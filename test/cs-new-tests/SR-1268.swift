// RUN: %target-typecheck-verify-swift

let x = 
["number":1, // expected-error{{heterogeneous collection literal could only be inferred to '[String : Any]'; add explicit type annotation if this is intentional}}
 "device":
	["name":"Device 1",
	"number":1,
	"exists":true,
	"ready":true,
	"devices":[],
	"groups":[],
	"scenes":[],
	"maxlevel":160,
	"minlevel":160,
	"physicalminlevel":160,
	"fadetimerate":1,
	"fadetime":506,
	"faderate":0.707107,
	"poweronlevel":254,
	"systemfailurelevel":254,
	"level":0,
	"lampfail":255,
	"limiterror":0,
	"lampignitioncounter":1,
	"actuatortype":166,
	"buserrors":0,
	"doubleaddressedprobability":0]]