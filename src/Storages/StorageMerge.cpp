#include <QueryPipeline/narrowPipe.h>
#include <QueryPipeline/QueryPipelineBuilder.h>
#include <Storages/StorageMerge.h>
#include <Storages/StorageFactory.h>
#include <Storages/StorageView.h>
#include <Storages/VirtualColumnUtils.h>
#include <Storages/AlterCommands.h>
#include <Storages/checkAndGetLiteralArgument.h>
#include <Interpreters/Context.h>
#include <Interpreters/TreeRewriter.h>
#include <Interpreters/ExpressionActions.h>
#include <Interpreters/evaluateConstantExpression.h>
#include <Interpreters/InterpreterSelectQuery.h>
#include <Interpreters/IdentifierSemantic.h>
#include <Interpreters/getHeaderForProcessingStage.h>
#include <Interpreters/addTypeConversionToAST.h>
#include <Interpreters/replaceAliasColumnsInQuery.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTExpressionList.h>
#include <DataTypes/DataTypeString.h>
#include <Columns/ColumnString.h>
#include <Common/typeid_cast.h>
#include <Common/checkStackSize.h>
#include <Processors/QueryPlan/ReadFromMergeTree.h>
#include <Processors/Sources/NullSource.h>
#include <Processors/QueryPlan/BuildQueryPipelineSettings.h>
#include <Processors/QueryPlan/Optimizations/QueryPlanOptimizationSettings.h>
#include <Processors/Transforms/MaterializingTransform.h>
#include <Processors/ConcatProcessor.h>
#include <Processors/Transforms/ExpressionTransform.h>
#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/Sources/SourceFromSingleChunk.h>
#include <Databases/IDatabase.h>
#include <base/range.h>
#include <algorithm>


namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int NOT_IMPLEMENTED;
    extern const int ILLEGAL_PREWHERE;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int SAMPLING_NOT_SUPPORTED;
    extern const int ALTER_OF_COLUMN_IS_FORBIDDEN;
    extern const int CANNOT_EXTRACT_TABLE_STRUCTURE;
}

StorageMerge::StorageMerge(
    const StorageID & table_id_,
    const ColumnsDescription & columns_,
    const String & comment,
    const String & source_database_name_or_regexp_,
    bool database_is_regexp_,
    const DBToTableSetMap & source_databases_and_tables_,
    ContextPtr context_)
    : IStorage(table_id_)
    , WithContext(context_->getGlobalContext())
    , source_database_regexp(source_database_name_or_regexp_)
    , source_databases_and_tables(source_databases_and_tables_)
    , source_database_name_or_regexp(source_database_name_or_regexp_)
    , database_is_regexp(database_is_regexp_)
{
    StorageInMemoryMetadata storage_metadata;
    storage_metadata.setColumns(columns_.empty() ? getColumnsDescriptionFromSourceTables() : columns_);
    storage_metadata.setComment(comment);
    setInMemoryMetadata(storage_metadata);
}

StorageMerge::StorageMerge(
    const StorageID & table_id_,
    const ColumnsDescription & columns_,
    const String & comment,
    const String & source_database_name_or_regexp_,
    bool database_is_regexp_,
    const String & source_table_regexp_,
    ContextPtr context_)
    : IStorage(table_id_)
    , WithContext(context_->getGlobalContext())
    , source_database_regexp(source_database_name_or_regexp_)
    , source_table_regexp(source_table_regexp_)
    , source_database_name_or_regexp(source_database_name_or_regexp_)
    , database_is_regexp(database_is_regexp_)
{
    StorageInMemoryMetadata storage_metadata;
    storage_metadata.setColumns(columns_.empty() ? getColumnsDescriptionFromSourceTables() : columns_);
    storage_metadata.setComment(comment);
    setInMemoryMetadata(storage_metadata);
}

ColumnsDescription StorageMerge::getColumnsDescriptionFromSourceTables() const
{
    auto table = getFirstTable([](auto && t) { return t; });
    if (!table)
        throw Exception{ErrorCodes::CANNOT_EXTRACT_TABLE_STRUCTURE, "There are no tables satisfied provided regexp, you must specify table structure manually"};
    return table->getInMemoryMetadataPtr()->getColumns();
}

template <typename F>
StoragePtr StorageMerge::getFirstTable(F && predicate) const
{
    auto database_table_iterators = getDatabaseIterators(getContext());

    for (auto & iterator : database_table_iterators)
    {
        while (iterator->isValid())
        {
            const auto & table = iterator->table();
            if (table.get() != this && predicate(table))
                return table;

            iterator->next();
        }
    }

    return {};
}

template <typename F>
void StorageMerge::forEachTable(F && func) const
{
    getFirstTable([&func](const auto & table)
    {
        func(table);
        return false;
    });
}

bool StorageMerge::isRemote() const
{
    auto first_remote_table = getFirstTable([](const StoragePtr & table) { return table && table->isRemote(); });
    return first_remote_table != nullptr;
}

bool StorageMerge::canMoveConditionsToPrewhere() const
{
    /// NOTE: This check is used during query analysis as condition for applying
    /// "move to PREWHERE" optimization. However, it contains a logical race:
    /// If new table that matches regexp for current storage and doesn't support PREWHERE
    /// will appear after this check and before calling "read" method, the optimized query may fail.
    /// Since it's quite rare case, we just ignore this possibility.
    const auto & table_doesnt_support_prewhere = getFirstTable([](const auto & table) { return !table->canMoveConditionsToPrewhere(); });
    bool can_move = (table_doesnt_support_prewhere == nullptr);

    if (!can_move)
        return false;

    if (!getInMemoryMetadataPtr())
        return false;

    std::unordered_map<std::string, const IDataType *> column_types;
    for (const auto & name_type : getInMemoryMetadataPtr()->getColumns().getAll())
    {
        column_types.emplace(name_type.name, name_type.type.get());
    }

    /// Check that all tables have the same column types, otherwise prewhere will fail
    forEachTable([&](const StoragePtr & table)
    {
        const auto & metadata_ptr = table->getInMemoryMetadataPtr();
        if (!metadata_ptr)
            can_move = false;

        if (!can_move)
            return;

        for (const auto & column : metadata_ptr->getColumns().getAll())
        {
            const auto * src_type = column_types[column.name];
            if (src_type && !src_type->equals(*column.type))
            {
                can_move = false;
                return;
            }
        }
    });

    return can_move;
}

bool StorageMerge::mayBenefitFromIndexForIn(const ASTPtr & left_in_operand, ContextPtr query_context, const StorageMetadataPtr & /*metadata_snapshot*/) const
{
    /// It's beneficial if it is true for at least one table.
    StorageListWithLocks selected_tables = getSelectedTables(query_context);

    size_t i = 0;
    for (const auto & table : selected_tables)
    {
        const auto & storage_ptr = std::get<1>(table);
        auto metadata_snapshot = storage_ptr->getInMemoryMetadataPtr();
        if (storage_ptr->mayBenefitFromIndexForIn(left_in_operand, query_context, metadata_snapshot))
            return true;

        ++i;
        /// For simplicity reasons, check only first ten tables.
        if (i > 10)
            break;
    }

    return false;
}


QueryProcessingStage::Enum StorageMerge::getQueryProcessingStage(
    ContextPtr local_context,
    QueryProcessingStage::Enum to_stage,
    const StorageSnapshotPtr &,
    SelectQueryInfo & query_info) const
{
    /// In case of JOIN the first stage (which includes JOIN)
    /// should be done on the initiator always.
    ///
    /// Since in case of JOIN query on shards will receive query without JOIN (and their columns).
    /// (see removeJoin())
    ///
    /// And for this we need to return FetchColumns.
    if (const auto * select = query_info.query->as<ASTSelectQuery>(); select && hasJoin(*select))
        return QueryProcessingStage::FetchColumns;

    auto stage_in_source_tables = QueryProcessingStage::FetchColumns;

    DatabaseTablesIterators database_table_iterators = getDatabaseIterators(local_context);

    size_t selected_table_size = 0;

    /// TODO: Find a way to support projections for StorageMerge
    query_info.ignore_projections = true;
    for (const auto & iterator : database_table_iterators)
    {
        while (iterator->isValid())
        {
            const auto & table = iterator->table();
            if (table && table.get() != this)
            {
                ++selected_table_size;
                stage_in_source_tables = std::max(
                    stage_in_source_tables,
                    table->getQueryProcessingStage(local_context, to_stage,
                        table->getStorageSnapshot(table->getInMemoryMetadataPtr(), local_context), query_info));
            }

            iterator->next();
        }
    }

    return selected_table_size == 1 ? stage_in_source_tables : std::min(stage_in_source_tables, QueryProcessingStage::WithMergeableState);
}


SelectQueryInfo getModifiedQueryInfo(
    const SelectQueryInfo & query_info, ContextPtr modified_context, const StorageID & current_storage_id, bool is_merge_engine)
{
    SelectQueryInfo modified_query_info = query_info;
    modified_query_info.query = query_info.query->clone();

    /// TODO: Analyzer syntax analyzer result
    if (modified_query_info.syntax_analyzer_result)
    {
        /// Original query could contain JOIN but we need only the first joined table and its columns.
        auto & modified_select = modified_query_info.query->as<ASTSelectQuery &>();
        TreeRewriterResult new_analyzer_res = *modified_query_info.syntax_analyzer_result;
        removeJoin(modified_select, new_analyzer_res, modified_context);
        modified_query_info.syntax_analyzer_result = std::make_shared<TreeRewriterResult>(std::move(new_analyzer_res));
    }

    if (!is_merge_engine)
    {
        VirtualColumnUtils::rewriteEntityInAst(modified_query_info.query, "_table", current_storage_id.table_name);
        VirtualColumnUtils::rewriteEntityInAst(modified_query_info.query, "_database", current_storage_id.database_name);
    }

    return modified_query_info;
}


void StorageMerge::read(
    QueryPlan & query_plan,
    const Names & column_names,
    const StorageSnapshotPtr & storage_snapshot,
    SelectQueryInfo & query_info,
    ContextPtr local_context,
    QueryProcessingStage::Enum processed_stage,
    const size_t max_block_size,
    size_t num_streams)
{
    /** Just in case, turn off optimization "transfer to PREWHERE",
      * since there is no certainty that it works when one of table is MergeTree and other is not.
      */
    auto modified_context = Context::createCopy(local_context);
    modified_context->setSetting("optimize_move_to_prewhere", false);

    bool has_database_virtual_column = false;
    bool has_table_virtual_column = false;
    Names real_column_names;
    real_column_names.reserve(column_names.size());

    for (const auto & column_name : column_names)
    {
        if (column_name == "_database" && isVirtualColumn(column_name, storage_snapshot->metadata))
            has_database_virtual_column = true;
        else if (column_name == "_table" && isVirtualColumn(column_name, storage_snapshot->metadata))
            has_table_virtual_column = true;
        else
            real_column_names.push_back(column_name);
    }

    StorageListWithLocks selected_tables
        = getSelectedTables(modified_context, query_info.query, has_database_virtual_column, has_table_virtual_column);

    InputOrderInfoPtr input_sorting_info;
    if (query_info.order_optimizer)
    {
        for (auto it = selected_tables.begin(); it != selected_tables.end(); ++it)
        {
            auto storage_ptr = std::get<1>(*it);
            auto storage_metadata_snapshot = storage_ptr->getInMemoryMetadataPtr();
            auto current_info = query_info.order_optimizer->getInputOrder(storage_metadata_snapshot, modified_context);
            if (it == selected_tables.begin())
                input_sorting_info = current_info;
            else if (!current_info || (input_sorting_info && *current_info != *input_sorting_info))
                input_sorting_info.reset();

            if (!input_sorting_info)
                break;
        }

        query_info.input_order_info = input_sorting_info;
    }

    query_plan.addInterpreterContext(modified_context);

    /// What will be result structure depending on query processed stage in source tables?
    Block common_header = getHeaderForProcessingStage(column_names, storage_snapshot, query_info, local_context, processed_stage);

    auto step = std::make_unique<ReadFromMerge>(
        common_header,
        std::move(selected_tables),
        real_column_names,
        has_database_virtual_column,
        has_table_virtual_column,
        max_block_size,
        num_streams,
        shared_from_this(),
        storage_snapshot,
        query_info,
        std::move(modified_context),
        processed_stage);

    query_plan.addStep(std::move(step));
}

ReadFromMerge::ReadFromMerge(
    Block common_header_,
    StorageListWithLocks selected_tables_,
    Names column_names_,
    bool has_database_virtual_column_,
    bool has_table_virtual_column_,
    size_t max_block_size,
    size_t num_streams,
    StoragePtr storage,
    StorageSnapshotPtr storage_snapshot,
    const SelectQueryInfo & query_info_,
    ContextMutablePtr context_,
    QueryProcessingStage::Enum processed_stage)
    : ISourceStep(DataStream{.header = common_header_})
    , required_max_block_size(max_block_size)
    , requested_num_streams(num_streams)
    , common_header(std::move(common_header_))
    , selected_tables(std::move(selected_tables_))
    , column_names(std::move(column_names_))
    , has_database_virtual_column(has_database_virtual_column_)
    , has_table_virtual_column(has_table_virtual_column_)
    , storage_merge(std::move(storage))
    , merge_storage_snapshot(std::move(storage_snapshot))
    , query_info(query_info_)
    , context(std::move(context_))
    , common_processed_stage(processed_stage)
{
}

void ReadFromMerge::initializePipeline(QueryPipelineBuilder & pipeline, const BuildQueryPipelineSettings &)
{
    if (selected_tables.empty())
    {
        pipeline.init(Pipe(std::make_shared<NullSource>(output_stream->header)));
        return;
    }

    size_t tables_count = selected_tables.size();
    Float64 num_streams_multiplier
        = std::min(static_cast<size_t>(tables_count), std::max(1UL, static_cast<size_t>(context->getSettingsRef().max_streams_multiplier_for_merge_tables)));
    size_t num_streams = static_cast<size_t>(requested_num_streams * num_streams_multiplier);
    size_t remaining_streams = num_streams;

    if (order_info)
    {
        query_info.input_order_info = order_info;
    }
    else if (query_info.order_optimizer)
    {
        InputOrderInfoPtr input_sorting_info;
        for (auto it = selected_tables.begin(); it != selected_tables.end(); ++it)
        {
            auto storage_ptr = std::get<1>(*it);
            auto storage_metadata_snapshot = storage_ptr->getInMemoryMetadataPtr();
            auto current_info = query_info.order_optimizer->getInputOrder(storage_metadata_snapshot, context);
            if (it == selected_tables.begin())
                input_sorting_info = current_info;
            else if (!current_info || (input_sorting_info && *current_info != *input_sorting_info))
                input_sorting_info.reset();

            if (!input_sorting_info)
                break;
        }

        query_info.input_order_info = input_sorting_info;
    }

    auto sample_block = merge_storage_snapshot->getMetadataForQuery()->getSampleBlock();

    std::vector<std::unique_ptr<QueryPipelineBuilder>> pipelines;
    QueryPlanResourceHolder resources;

    for (const auto & table : selected_tables)
    {
        size_t current_need_streams = tables_count >= num_streams ? 1 : (num_streams / tables_count);
        size_t current_streams = std::min(current_need_streams, remaining_streams);
        remaining_streams -= current_streams;
        current_streams = std::max(static_cast<size_t>(1), current_streams);

        const auto & storage = std::get<1>(table);

        /// If sampling requested, then check that table supports it.
        if (query_info.query->as<ASTSelectQuery>()->sampleSize() && !storage->supportsSampling())
            throw Exception("Illegal SAMPLE: table doesn't support sampling", ErrorCodes::SAMPLING_NOT_SUPPORTED);

        Aliases aliases;
        auto storage_metadata_snapshot = storage->getInMemoryMetadataPtr();
        auto storage_columns = storage_metadata_snapshot->getColumns();
        auto nested_storage_snaphsot = storage->getStorageSnapshot(storage_metadata_snapshot, context);

        auto modified_query_info = getModifiedQueryInfo(query_info, context, storage->getStorageID(), storage->as<StorageMerge>());
        auto syntax_result = TreeRewriter(context).analyzeSelect(
            modified_query_info.query, TreeRewriterResult({}, storage, nested_storage_snaphsot));

        Names column_names_as_aliases;
        bool with_aliases = common_processed_stage == QueryProcessingStage::FetchColumns && !storage_columns.getAliases().empty();
        if (with_aliases)
        {
            ASTPtr required_columns_expr_list = std::make_shared<ASTExpressionList>();
            ASTPtr column_expr;

            for (const auto & column : column_names)
            {
                const auto column_default = storage_columns.getDefault(column);
                bool is_alias = column_default && column_default->kind == ColumnDefaultKind::Alias;

                if (is_alias)
                {
                    column_expr = column_default->expression->clone();
                    replaceAliasColumnsInQuery(column_expr, storage_metadata_snapshot->getColumns(),
                                               syntax_result->array_join_result_to_source, context);

                    auto column_description = storage_columns.get(column);
                    column_expr = addTypeConversionToAST(std::move(column_expr), column_description.type->getName(),
                                                         storage_metadata_snapshot->getColumns().getAll(), context);
                    column_expr = setAlias(column_expr, column);

                    auto type = sample_block.getByName(column).type;
                    aliases.push_back({ .name = column, .type = type, .expression = column_expr->clone() });
                }
                else
                    column_expr = std::make_shared<ASTIdentifier>(column);

                required_columns_expr_list->children.emplace_back(std::move(column_expr));
            }

            syntax_result = TreeRewriter(context).analyze(
                required_columns_expr_list, storage_columns.getAllPhysical(), storage, storage->getStorageSnapshot(storage_metadata_snapshot, context));

            auto alias_actions = ExpressionAnalyzer(required_columns_expr_list, syntax_result, context).getActionsDAG(true);

            column_names_as_aliases = alias_actions->getRequiredColumns().getNames();
            if (column_names_as_aliases.empty())
                column_names_as_aliases.push_back(ExpressionActions::getSmallestColumn(storage_metadata_snapshot->getColumns().getAllPhysical()).name);
        }

        auto source_pipeline = createSources(
            nested_storage_snaphsot,
            modified_query_info,
            common_processed_stage,
            required_max_block_size,
            common_header,
            aliases,
            table,
            column_names_as_aliases.empty() ? column_names : column_names_as_aliases,
            context,
            current_streams);

        if (source_pipeline && source_pipeline->initialized())
        {
            resources.storage_holders.push_back(std::get<1>(table));
            resources.table_locks.push_back(std::get<2>(table));

            pipelines.emplace_back(std::move(source_pipeline));
        }
    }

    if (pipelines.empty())
    {
        pipeline.init(Pipe(std::make_shared<NullSource>(output_stream->header)));
        return;
    }

    pipeline = QueryPipelineBuilder::unitePipelines(std::move(pipelines));

    if (!query_info.input_order_info)
        // It's possible to have many tables read from merge, resize(num_streams) might open too many files at the same time.
        // Using narrowPipe instead. But in case of reading in order of primary key, we cannot do it,
        // because narrowPipe doesn't preserve order.
        pipeline.narrow(num_streams);

    pipeline.addResources(std::move(resources));
}

QueryPipelineBuilderPtr ReadFromMerge::createSources(
    const StorageSnapshotPtr & storage_snapshot,
    SelectQueryInfo & modified_query_info,
    const QueryProcessingStage::Enum & processed_stage,
    const UInt64 max_block_size,
    const Block & header,
    const Aliases & aliases,
    const StorageWithLockAndName & storage_with_lock,
    Names & real_column_names,
    ContextMutablePtr modified_context,
    size_t streams_num,
    bool concat_streams)
{
    const auto & [database_name, storage, _, table_name] = storage_with_lock;
    auto & modified_select = modified_query_info.query->as<ASTSelectQuery &>();

    QueryPipelineBuilderPtr builder;

    if (!storage)
    {
        return std::make_unique<QueryPipelineBuilder>(InterpreterSelectQuery(
            modified_query_info.query, modified_context,
            Pipe(std::make_shared<SourceFromSingleChunk>(header)),
            SelectQueryOptions(processed_stage).analyze()).buildQueryPipeline());
    }

    bool final = false;
    if (modified_query_info.table_expression_modifiers)
        final = modified_query_info.table_expression_modifiers->hasFinal();
    else
        final = modified_select.final();

    if (!final && storage->needRewriteQueryWithFinal(real_column_names))
    {
        /// NOTE: It may not work correctly in some cases, because query was analyzed without final.
        /// However, it's needed for MaterializedMySQL and it's unlikely that someone will use it with Merge tables.
        modified_select.setFinal();
    }

    auto storage_stage
        = storage->getQueryProcessingStage(modified_context, QueryProcessingStage::Complete, storage_snapshot, modified_query_info);
    if (processed_stage <= storage_stage)
    {
        /// If there are only virtual columns in query, you must request at least one other column.
        if (real_column_names.empty())
            real_column_names.push_back(ExpressionActions::getSmallestColumn(storage_snapshot->metadata->getColumns().getAllPhysical()).name);

        QueryPlan plan;
        if (StorageView * view = dynamic_cast<StorageView *>(storage.get()))
        {
            /// For view storage, we need to rewrite the `modified_query_info.view_query` to optimize read.
            /// The most intuitive way is to use InterpreterSelectQuery.

            /// Intercept the settings
            modified_context->setSetting("max_threads", streams_num);
            modified_context->setSetting("max_streams_to_max_threads_ratio", 1);
            modified_context->setSetting("max_block_size", max_block_size);

            InterpreterSelectQuery(
                modified_query_info.query, modified_context, storage, view->getInMemoryMetadataPtr(), SelectQueryOptions(processed_stage))
                .buildQueryPlan(plan);
        }
        else
        {
            storage->read(
                plan,
                real_column_names,
                storage_snapshot,
                modified_query_info,
                modified_context,
                processed_stage,
                max_block_size,
                UInt32(streams_num));

        }

        if (!plan.isInitialized())
            return {};

        if (auto * read_from_merge_tree = typeid_cast<ReadFromMergeTree *>(plan.getRootNode()->step.get()))
            read_from_merge_tree->addFilterNodes(added_filter_nodes);

        builder = plan.buildQueryPipeline(
            QueryPlanOptimizationSettings::fromContext(modified_context),
            BuildQueryPipelineSettings::fromContext(modified_context));
    }
    else if (processed_stage > storage_stage)
    {
        modified_select.replaceDatabaseAndTable(database_name, table_name);

        /// Maximum permissible parallelism is streams_num
        modified_context->setSetting("max_threads", streams_num);
        modified_context->setSetting("max_streams_to_max_threads_ratio", 1);

        /// TODO: Find a way to support projections for StorageMerge
        InterpreterSelectQuery interpreter{
            modified_query_info.query, modified_context, SelectQueryOptions(processed_stage).ignoreProjections()};

        builder = std::make_unique<QueryPipelineBuilder>(interpreter.buildQueryPipeline());

        /** Materialization is needed, since from distributed storage the constants come materialized.
          * If you do not do this, different types (Const and non-Const) columns will be produced in different threads,
          * And this is not allowed, since all code is based on the assumption that in the block stream all types are the same.
          */
        builder->addSimpleTransform([](const Block & stream_header) { return std::make_shared<MaterializingTransform>(stream_header); });
    }

    if (builder->initialized())
    {
        if (concat_streams && builder->getNumStreams() > 1)
        {
            // It's possible to have many tables read from merge, resize(1) might open too many files at the same time.
            // Using concat instead.
            builder->addTransform(std::make_shared<ConcatProcessor>(builder->getHeader(), builder->getNumStreams()));
        }

        /// Add virtual columns if we don't already have them.

        Block pipe_header = builder->getHeader();

        if (has_database_virtual_column && !pipe_header.has("_database"))
        {
            ColumnWithTypeAndName column;
            column.name = "_database";
            column.type = std::make_shared<DataTypeString>();
            column.column = column.type->createColumnConst(0, Field(database_name));

            auto adding_column_dag = ActionsDAG::makeAddingColumnActions(std::move(column));
            auto adding_column_actions = std::make_shared<ExpressionActions>(
                std::move(adding_column_dag),
                ExpressionActionsSettings::fromContext(modified_context, CompileExpressions::yes));

            builder->addSimpleTransform([&](const Block & stream_header)
            {
                return std::make_shared<ExpressionTransform>(stream_header, adding_column_actions);
            });
        }

        if (has_table_virtual_column && !pipe_header.has("_table"))
        {
            ColumnWithTypeAndName column;
            column.name = "_table";
            column.type = std::make_shared<DataTypeString>();
            column.column = column.type->createColumnConst(0, Field(table_name));

            auto adding_column_dag = ActionsDAG::makeAddingColumnActions(std::move(column));
            auto adding_column_actions = std::make_shared<ExpressionActions>(
                std::move(adding_column_dag),
                ExpressionActionsSettings::fromContext(modified_context, CompileExpressions::yes));

            builder->addSimpleTransform([&](const Block & stream_header)
            {
                return std::make_shared<ExpressionTransform>(stream_header, adding_column_actions);
            });
        }

        /// Subordinary tables could have different but convertible types, like numeric types of different width.
        /// We must return streams with structure equals to structure of Merge table.
        convertingSourceStream(header, storage_snapshot->metadata, aliases, modified_context, *builder);
    }

    return builder;
}

StorageMerge::StorageListWithLocks StorageMerge::getSelectedTables(
    ContextPtr query_context,
    const ASTPtr & query /* = nullptr */,
    bool filter_by_database_virtual_column /* = false */,
    bool filter_by_table_virtual_column /* = false */) const
{
    assert(!filter_by_database_virtual_column || !filter_by_table_virtual_column || query);

    const Settings & settings = query_context->getSettingsRef();
    StorageListWithLocks selected_tables;
    DatabaseTablesIterators database_table_iterators = getDatabaseIterators(getContext());

    MutableColumnPtr database_name_virtual_column;
    MutableColumnPtr table_name_virtual_column;
    if (filter_by_database_virtual_column)
    {
        database_name_virtual_column = ColumnString::create();
    }

    if (filter_by_table_virtual_column)
    {
        table_name_virtual_column = ColumnString::create();
    }

    for (const auto & iterator : database_table_iterators)
    {
        if (filter_by_database_virtual_column)
            database_name_virtual_column->insert(iterator->databaseName());
        while (iterator->isValid())
        {
            StoragePtr storage = iterator->table();
            if (!storage)
                continue;

            if (query && query->as<ASTSelectQuery>()->prewhere() && !storage->supportsPrewhere())
                throw Exception("Storage " + storage->getName() + " doesn't support PREWHERE.", ErrorCodes::ILLEGAL_PREWHERE);

            if (storage.get() != this)
            {
                auto table_lock = storage->lockForShare(query_context->getCurrentQueryId(), settings.lock_acquire_timeout);
                selected_tables.emplace_back(iterator->databaseName(), storage, std::move(table_lock), iterator->name());
                if (filter_by_table_virtual_column)
                    table_name_virtual_column->insert(iterator->name());
            }

            iterator->next();
        }
    }

    if (filter_by_database_virtual_column)
    {
        /// Filter names of selected tables if there is a condition on "_database" virtual column in WHERE clause
        Block virtual_columns_block
            = Block{ColumnWithTypeAndName(std::move(database_name_virtual_column), std::make_shared<DataTypeString>(), "_database")};
        VirtualColumnUtils::filterBlockWithQuery(query, virtual_columns_block, query_context);
        auto values = VirtualColumnUtils::extractSingleValueFromBlock<String>(virtual_columns_block, "_database");

        /// Remove unused databases from the list
        selected_tables.remove_if([&](const auto & elem) { return values.find(std::get<0>(elem)) == values.end(); });
    }

    if (filter_by_table_virtual_column)
    {
        /// Filter names of selected tables if there is a condition on "_table" virtual column in WHERE clause
        Block virtual_columns_block = Block{ColumnWithTypeAndName(std::move(table_name_virtual_column), std::make_shared<DataTypeString>(), "_table")};
        VirtualColumnUtils::filterBlockWithQuery(query, virtual_columns_block, query_context);
        auto values = VirtualColumnUtils::extractSingleValueFromBlock<String>(virtual_columns_block, "_table");

        /// Remove unused tables from the list
        selected_tables.remove_if([&](const auto & elem) { return values.find(std::get<3>(elem)) == values.end(); });
    }

    return selected_tables;
}

DatabaseTablesIteratorPtr StorageMerge::getDatabaseIterator(const String & database_name, ContextPtr local_context) const
{
    auto database = DatabaseCatalog::instance().getDatabase(database_name);

    auto table_name_match = [this, database_name](const String & table_name_) -> bool
    {
        if (source_databases_and_tables)
        {
            if (auto it = source_databases_and_tables->find(database_name); it != source_databases_and_tables->end())
                return it->second.contains(table_name_);
            else
                return false;
        }
        else
            return source_table_regexp->match(table_name_);
    };

    return database->getTablesIterator(local_context, table_name_match);
}

StorageMerge::DatabaseTablesIterators StorageMerge::getDatabaseIterators(ContextPtr local_context) const
{
    try
    {
        checkStackSize();
    }
    catch (Exception & e)
    {
        e.addMessage("while getting table iterator of Merge table. Maybe caused by two Merge tables that will endlessly try to read each other's data");
        throw;
    }

    DatabaseTablesIterators database_table_iterators;

    /// database_name argument is not a regexp
    if (!database_is_regexp)
        database_table_iterators.emplace_back(getDatabaseIterator(source_database_name_or_regexp, local_context));

    /// database_name argument is a regexp
    else
    {
        auto databases = DatabaseCatalog::instance().getDatabases();

        for (const auto & db : databases)
        {
            if (source_database_regexp->match(db.first))
                database_table_iterators.emplace_back(getDatabaseIterator(db.first, local_context));
        }
    }

    return database_table_iterators;
}


void StorageMerge::checkAlterIsPossible(const AlterCommands & commands, ContextPtr local_context) const
{
    auto name_deps = getDependentViewsByColumn(local_context);
    for (const auto & command : commands)
    {
        if (command.type != AlterCommand::Type::ADD_COLUMN && command.type != AlterCommand::Type::MODIFY_COLUMN
            && command.type != AlterCommand::Type::DROP_COLUMN && command.type != AlterCommand::Type::COMMENT_COLUMN
            && command.type != AlterCommand::Type::COMMENT_TABLE)
            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Alter of type '{}' is not supported by storage {}",
                command.type, getName());

        if (command.type == AlterCommand::Type::DROP_COLUMN && !command.clear)
        {
            const auto & deps_mv = name_deps[command.column_name];
            if (!deps_mv.empty())
            {
                throw Exception(ErrorCodes::ALTER_OF_COLUMN_IS_FORBIDDEN,
                    "Trying to ALTER DROP column {} which is referenced by materialized view {}",
                    backQuoteIfNeed(command.column_name), toString(deps_mv));
            }
        }
    }
}

void StorageMerge::alter(
    const AlterCommands & params, ContextPtr local_context, AlterLockHolder &)
{
    auto table_id = getStorageID();

    StorageInMemoryMetadata storage_metadata = getInMemoryMetadata();
    params.apply(storage_metadata, local_context);
    DatabaseCatalog::instance().getDatabase(table_id.database_name)->alterTable(local_context, table_id, storage_metadata);
    setInMemoryMetadata(storage_metadata);
}

void ReadFromMerge::convertingSourceStream(
    const Block & header,
    const StorageMetadataPtr & metadata_snapshot,
    const Aliases & aliases,
    ContextPtr local_context,
    QueryPipelineBuilder & builder)
{
    Block before_block_header = builder.getHeader();

    auto storage_sample_block = metadata_snapshot->getSampleBlock();
    auto pipe_columns = builder.getHeader().getNamesAndTypesList();

    for (const auto & alias : aliases)
    {
        pipe_columns.emplace_back(NameAndTypePair(alias.name, alias.type));
        ASTPtr expr = alias.expression;
        auto syntax_result = TreeRewriter(local_context).analyze(expr, pipe_columns);
        auto expression_analyzer = ExpressionAnalyzer{alias.expression, syntax_result, local_context};

        auto dag = std::make_shared<ActionsDAG>(pipe_columns);
        auto actions_dag = expression_analyzer.getActionsDAG(true, false);
        auto actions = std::make_shared<ExpressionActions>(actions_dag, ExpressionActionsSettings::fromContext(local_context, CompileExpressions::yes));

        builder.addSimpleTransform([&](const Block & stream_header)
        {
            return std::make_shared<ExpressionTransform>(stream_header, actions);
        });
    }

    {
        auto convert_actions_dag = ActionsDAG::makeConvertingActions(builder.getHeader().getColumnsWithTypeAndName(),
                                                                     header.getColumnsWithTypeAndName(),
                                                                     ActionsDAG::MatchColumnsMode::Name);
        auto actions = std::make_shared<ExpressionActions>(
            convert_actions_dag,
            ExpressionActionsSettings::fromContext(local_context, CompileExpressions::yes));

        builder.addSimpleTransform([&](const Block & stream_header)
        {
            return std::make_shared<ExpressionTransform>(stream_header, actions);
        });
    }
}

IStorage::ColumnSizeByName StorageMerge::getColumnSizes() const
{
    ColumnSizeByName column_sizes;

    forEachTable([&](const auto & table)
    {
        for (const auto & [name, size] : table->getColumnSizes())
            column_sizes[name].add(size);
    });

    return column_sizes;
}


std::tuple<bool /* is_regexp */, ASTPtr> StorageMerge::evaluateDatabaseName(const ASTPtr & node, ContextPtr context_)
{
    if (const auto * func = node->as<ASTFunction>(); func && func->name == "REGEXP")
    {
        if (func->arguments->children.size() != 1)
            throw Exception("REGEXP in Merge ENGINE takes only one argument", ErrorCodes::BAD_ARGUMENTS);

        auto * literal = func->arguments->children[0]->as<ASTLiteral>();
        if (!literal || literal->value.getType() != Field::Types::Which::String || literal->value.safeGet<String>().empty())
            throw Exception("Argument for REGEXP in Merge ENGINE should be a non empty String Literal", ErrorCodes::BAD_ARGUMENTS);

        return {true, func->arguments->children[0]};
    }

    auto ast = evaluateConstantExpressionForDatabaseName(node, context_);
    return {false, ast};
}


void registerStorageMerge(StorageFactory & factory)
{
    factory.registerStorage("Merge", [](const StorageFactory::Arguments & args)
    {
        /** In query, the name of database is specified as table engine argument which contains source tables,
          *  as well as regex for source-table names.
          */

        ASTs & engine_args = args.engine_args;

        if (engine_args.size() != 2)
            throw Exception("Storage Merge requires exactly 2 parameters"
                " - name of source database and regexp for table names.",
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        auto [is_regexp, database_ast] = StorageMerge::evaluateDatabaseName(engine_args[0], args.getLocalContext());

        if (!is_regexp)
            engine_args[0] = database_ast;

        String source_database_name_or_regexp = checkAndGetLiteralArgument<String>(database_ast, "database_name");

        engine_args[1] = evaluateConstantExpressionAsLiteral(engine_args[1], args.getLocalContext());
        String table_name_regexp = checkAndGetLiteralArgument<String>(engine_args[1], "table_name_regexp");

        return std::make_shared<StorageMerge>(
            args.table_id, args.columns, args.comment, source_database_name_or_regexp, is_regexp, table_name_regexp, args.getContext());
    },
    {
        .supports_schema_inference = true
    });
}

NamesAndTypesList StorageMerge::getVirtuals() const
{
    NamesAndTypesList virtuals{{"_database", std::make_shared<DataTypeString>()}, {"_table", std::make_shared<DataTypeString>()}};

    auto first_table = getFirstTable([](auto && table) { return table; });
    if (first_table)
    {
        auto table_virtuals = first_table->getVirtuals();
        virtuals.insert(virtuals.end(), table_virtuals.begin(), table_virtuals.end());
    }

    return virtuals;
}
}
