#pragma once

static const uint16_t dt_srgb_tone_curve_values[] = {
  0,     5,     10,    15,    20,    25,    30,    35,    40,    45,    50,    55,    59,    64,    69,
  74,    79,    84,    89,    94,    99,    104,   109,   114,   119,   124,   129,   134,   139,   144,
  149,   154,   159,   164,   169,   174,   178,   183,   188,   193,   198,   203,   208,   213,   219,
  224,   229,   235,   240,   246,   251,   257,   263,   269,   275,   281,   287,   293,   299,   306,
  312,   318,   325,   332,   338,   345,   352,   359,   366,   373,   380,   387,   395,   402,   410,
  417,   425,   433,   441,   449,   457,   465,   473,   481,   489,   498,   506,   515,   524,   532,
  541,   550,   559,   568,   577,   587,   596,   605,   615,   625,   634,   644,   654,   664,   674,
  684,   694,   705,   715,   725,   736,   747,   757,   768,   779,   790,   801,   813,   824,   835,
  847,   858,   870,   882,   894,   906,   918,   930,   942,   954,   967,   979,   992,   1004,  1017,
  1030,  1043,  1056,  1069,  1083,  1096,  1109,  1123,  1137,  1150,  1164,  1178,  1192,  1206,  1220,
  1235,  1249,  1264,  1278,  1293,  1308,  1323,  1338,  1353,  1368,  1383,  1399,  1414,  1430,  1446,
  1461,  1477,  1493,  1509,  1526,  1542,  1558,  1575,  1591,  1608,  1625,  1642,  1659,  1676,  1693,
  1711,  1728,  1745,  1763,  1781,  1799,  1817,  1835,  1853,  1871,  1889,  1908,  1926,  1945,  1964,
  1983,  2002,  2021,  2040,  2059,  2079,  2098,  2118,  2138,  2158,  2178,  2198,  2218,  2238,  2258,
  2279,  2299,  2320,  2341,  2362,  2383,  2404,  2425,  2447,  2468,  2490,  2511,  2533,  2555,  2577,
  2599,  2621,  2644,  2666,  2689,  2712,  2734,  2757,  2780,  2803,  2827,  2850,  2873,  2897,  2921,
  2944,  2968,  2992,  3016,  3041,  3065,  3090,  3114,  3139,  3164,  3189,  3214,  3239,  3264,  3289,
  3315,  3341,  3366,  3392,  3418,  3444,  3470,  3497,  3523,  3550,  3576,  3603,  3630,  3657,  3684,
  3711,  3739,  3766,  3794,  3822,  3849,  3877,  3905,  3934,  3962,  3990,  4019,  4047,  4076,  4105,
  4134,  4163,  4193,  4222,  4251,  4281,  4311,  4341,  4371,  4401,  4431,  4461,  4492,  4522,  4553,
  4584,  4615,  4646,  4677,  4708,  4740,  4771,  4803,  4835,  4867,  4899,  4931,  4963,  4995,  5028,
  5061,  5093,  5126,  5159,  5193,  5226,  5259,  5293,  5326,  5360,  5394,  5428,  5462,  5496,  5531,
  5565,  5600,  5635,  5670,  5705,  5740,  5775,  5810,  5846,  5882,  5917,  5953,  5989,  6025,  6062,
  6098,  6135,  6171,  6208,  6245,  6282,  6319,  6357,  6394,  6432,  6469,  6507,  6545,  6583,  6621,
  6660,  6698,  6737,  6775,  6814,  6853,  6892,  6932,  6971,  7011,  7050,  7090,  7130,  7170,  7210,
  7250,  7291,  7331,  7372,  7413,  7454,  7495,  7536,  7577,  7619,  7660,  7702,  7744,  7786,  7828,
  7870,  7913,  7955,  7998,  8041,  8084,  8127,  8170,  8213,  8257,  8300,  8344,  8388,  8432,  8476,
  8520,  8565,  8609,  8654,  8699,  8743,  8789,  8834,  8879,  8925,  8970,  9016,  9062,  9108,  9154,
  9200,  9247,  9293,  9340,  9387,  9434,  9481,  9528,  9576,  9623,  9671,  9719,  9767,  9815,  9863,
  9911,  9960,  10008, 10057, 10106, 10155, 10204, 10253, 10303, 10353, 10402, 10452, 10502, 10552, 10603,
  10653, 10704, 10754, 10805, 10856, 10907, 10959, 11010, 11062, 11113, 11165, 11217, 11269, 11321, 11374,
  11426, 11479, 11532, 11585, 11638, 11691, 11745, 11798, 11852, 11906, 11959, 12014, 12068, 12122, 12177,
  12231, 12286, 12341, 12396, 12452, 12507, 12562, 12618, 12674, 12730, 12786, 12842, 12899, 12955, 13012,
  13069, 13126, 13183, 13240, 13297, 13355, 13413, 13470, 13528, 13587, 13645, 13703, 13762, 13821, 13879,
  13938, 13998, 14057, 14116, 14176, 14236, 14295, 14356, 14416, 14476, 14536, 14597, 14658, 14719, 14780,
  14841, 14902, 14964, 15026, 15087, 15149, 15211, 15274, 15336, 15399, 15461, 15524, 15587, 15650, 15713,
  15777, 15840, 15904, 15968, 16032, 16096, 16161, 16225, 16290, 16354, 16419, 16484, 16550, 16615, 16681,
  16746, 16812, 16878, 16944, 17010, 17077, 17143, 17210, 17277, 17344, 17411, 17479, 17546, 17614, 17682,
  17749, 17818, 17886, 17954, 18023, 18091, 18160, 18229, 18299, 18368, 18437, 18507, 18577, 18647, 18717,
  18787, 18857, 18928, 18999, 19069, 19140, 19212, 19283, 19354, 19426, 19498, 19570, 19642, 19714, 19786,
  19859, 19932, 20005, 20078, 20151, 20224, 20297, 20371, 20445, 20519, 20593, 20667, 20742, 20816, 20891,
  20966, 21041, 21116, 21191, 21267, 21343, 21418, 21494, 21570, 21647, 21723, 21800, 21877, 21954, 22031,
  22108, 22185, 22263, 22340, 22418, 22496, 22575, 22653, 22731, 22810, 22889, 22968, 23047, 23126, 23206,
  23285, 23365, 23445, 23525, 23605, 23686, 23766, 23847, 23928, 24009, 24090, 24172, 24253, 24335, 24417,
  24499, 24581, 24663, 24746, 24828, 24911, 24994, 25077, 25161, 25244, 25328, 25411, 25495, 25579, 25664,
  25748, 25833, 25917, 26002, 26087, 26173, 26258, 26344, 26429, 26515, 26601, 26687, 26774, 26860, 26947,
  27034, 27121, 27208, 27295, 27383, 27471, 27559, 27647, 27735, 27823, 27912, 28000, 28089, 28178, 28267,
  28356, 28446, 28536, 28625, 28715, 28806, 28896, 28986, 29077, 29168, 29259, 29350, 29441, 29533, 29624,
  29716, 29808, 29900, 29992, 30085, 30177, 30270, 30363, 30456, 30550, 30643, 30737, 30830, 30924, 31018,
  31113, 31207, 31302, 31397, 31492, 31587, 31682, 31777, 31873, 31969, 32065, 32161, 32257, 32354, 32450,
  32547, 32644, 32741, 32839, 32936, 33034, 33131, 33229, 33328, 33426, 33524, 33623, 33722, 33821, 33920,
  34019, 34119, 34219, 34318, 34418, 34519, 34619, 34719, 34820, 34921, 35022, 35123, 35225, 35326, 35428,
  35530, 35632, 35734, 35836, 35939, 36042, 36145, 36248, 36351, 36454, 36558, 36662, 36766, 36870, 36974,
  37078, 37183, 37288, 37393, 37498, 37603, 37709, 37814, 37920, 38026, 38132, 38239, 38345, 38452, 38559,
  38666, 38773, 38880, 38988, 39096, 39204, 39312, 39420, 39528, 39637, 39746, 39855, 39964, 40073, 40183,
  40292, 40402, 40512, 40622, 40733, 40843, 40954, 41065, 41176, 41287, 41398, 41510, 41622, 41734, 41846,
  41958, 42070, 42183, 42296, 42409, 42522, 42635, 42749, 42862, 42976, 43090, 43204, 43319, 43433, 43548,
  43663, 43778, 43893, 44009, 44124, 44240, 44356, 44472, 44589, 44705, 44822, 44939, 45056, 45173, 45290,
  45408, 45526, 45643, 45762, 45880, 45998, 46117, 46236, 46355, 46474, 46593, 46713, 46832, 46952, 47072,
  47193, 47313, 47434, 47554, 47675, 47797, 47918, 48039, 48161, 48283, 48405, 48527, 48650, 48772, 48895,
  49018, 49141, 49264, 49388, 49511, 49635, 49759, 49883, 50008, 50132, 50257, 50382, 50507, 50632, 50758,
  50883, 51009, 51135, 51261, 51388, 51514, 51641, 51768, 51895, 52022, 52150, 52277, 52405, 52533, 52661,
  52790, 52918, 53047, 53176, 53305, 53434, 53564, 53694, 53823, 53953, 54084, 54214, 54345, 54475, 54606,
  54737, 54869, 55000, 55132, 55264, 55396, 55528, 55660, 55793, 55926, 56059, 56192, 56325, 56458, 56592,
  56726, 56860, 56994, 57129, 57263, 57398, 57533, 57668, 57804, 57939, 58075, 58211, 58347, 58483, 58620,
  58756, 58893, 59030, 59167, 59305, 59442, 59580, 59718, 59856, 59995, 60133, 60272, 60411, 60550, 60689,
  60828, 60968, 61108, 61248, 61388, 61528, 61669, 61810, 61951, 62092, 62233, 62375, 62516, 62658, 62800,
  62942, 63085, 63227, 63370, 63513, 63656, 63800, 63943, 64087, 64231, 64375, 64519, 64664, 64809, 64954,
  65099, 65244, 65389, 65535
};

static const guint dt_srgb_tone_curve_values_n = G_N_ELEMENTS(dt_srgb_tone_curve_values);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
