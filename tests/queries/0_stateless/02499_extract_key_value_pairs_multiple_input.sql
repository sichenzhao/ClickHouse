-- { echoOn }
----- Enclosed elements tests -----

-- Expected output: {'name':'Neymar','team':'psg','age':'30','favorite_movie':'','height':'1.75'}
SELECT extractKeyValuePairs('"name": "Neymar", "age": 30, team: "psg", "favorite_movie": "", height: 1.75', '\\', ':', ',', '"', '.');

----- Escaping tests -----

-- Expected output: {'age':'30'}
SELECT extractKeyValuePairs('na,me,: neymar, age:30', '\\', ':', ',', '"');

-- Expected output: {'age':'30'}
SELECT extractKeyValuePairs('na$me,: neymar, age:30', '\\', ':', ',', '"');

-- Expected output: {'name':'neymar','favorite_quote':'Premature optimization is the r$$t of all evil','age':'30'}
select extractKeyValuePairs('name: neymar, favorite_quote: Premature\\ optimization\\ is\\ the\\ r\\$\\$t\\ of\\ all\\ evil, age:30', '\\', ':', ',', '"');

----- Mix Strings -----

-- Expected output: {'no:me':'neymar','age':'30','height':'1.75','school':'lupe picasso','team':'psg'}
select extractKeyValuePairs('9 ads =nm,  no\:me: neymar, age: 30, daojmskdpoa and a  height:   1.75, school: lupe\ picasso, team: psg,', '\\', ':', ',', '"', '.');

-- Expected output: {'XNFHGSSF_RHRUZHVBS_KWBT':'F'}
SELECT extractKeyValuePairs('XNFHGSSF_RHRUZHVBS_KWBT: F,', '\\', ':', ',', '"');

----- Allow list special value characters -----

-- Expected output: {'some_key':'@dolla%sign$'}
SELECT extractKeyValuePairs('some_key: @dolla%sign$,', '\\', ':', ',', '"', '$@%');
