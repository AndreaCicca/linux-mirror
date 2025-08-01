// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip RK3288 VPU codec driver
 *
 * Copyright (c) 2014 Rockchip Electronics Co., Ltd.
 *	Hertz Wong <hertz.wong@rock-chips.com>
 *	Herman Chen <herman.chen@rock-chips.com>
 *
 * Copyright (C) 2014 Google, Inc.
 *	Tomasz Figa <tfiga@chromium.org>
 */

#include <linux/types.h>
#include <media/v4l2-h264.h>
#include <media/v4l2-mem2mem.h>

#include "hantro.h"
#include "hantro_hw.h"

/* Size with u32 units. */
#define CABAC_INIT_BUFFER_SIZE		(460 * 2)
#define POC_BUFFER_SIZE			34
#define SCALING_LIST_SIZE		(6 * 16 + 2 * 64)

/*
 * For valid and long term reference marking, index are reversed, so bit 31
 * indicates the status of the picture 0.
 */
#define REF_BIT(i)			BIT(32 - 1 - (i))

/* Data structure describing auxiliary buffer format. */
struct hantro_h264_dec_priv_tbl {
	u32 cabac_table[CABAC_INIT_BUFFER_SIZE];
	u32 poc[POC_BUFFER_SIZE];
	u8 scaling_list[SCALING_LIST_SIZE];
};

/*
 * Constant CABAC table.
 * From drivers/media/platform/rk3288-vpu/rk3288_vpu_hw_h264d.c
 * in https://chromium.googlesource.com/chromiumos/third_party/kernel,
 * chromeos-3.14 branch.
 */
static const u32 h264_cabac_table[] = {
	0x14f10236, 0x034a14f1, 0x0236034a, 0xe47fe968, 0xfa35ff36, 0x07330000,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x0029003f, 0x003f003f, 0xf7530456, 0x0061f948, 0x0d29033e, 0x000b0137,
	0x0045ef7f, 0xf3660052, 0xf94aeb6b, 0xe57fe17f, 0xe87fee5f, 0xe57feb72,
	0xe27fef7b, 0xf473f07a, 0xf573f43f, 0xfe44f154, 0xf368fd46, 0xf85df65a,
	0xe27fff4a, 0xfa61f95b, 0xec7ffc38, 0xfb52f94c, 0xea7df95d, 0xf557fd4d,
	0xfb47fc3f, 0xfc44f454, 0xf93ef941, 0x083d0538, 0xfe420140, 0x003dfe4e,
	0x01320734, 0x0a23002c, 0x0b26012d, 0x002e052c, 0x1f110133, 0x07321c13,
	0x10210e3e, 0xf36cf164, 0xf365f35b, 0xf45ef658, 0xf054f656, 0xf953f357,
	0xed5e0146, 0x0048fb4a, 0x123bf866, 0xf164005f, 0xfc4b0248, 0xf54bfd47,
	0x0f2ef345, 0x003e0041, 0x1525f148, 0x09391036, 0x003e0c48, 0x18000f09,
	0x08190d12, 0x0f090d13, 0x0a250c12, 0x061d1421, 0x0f1e042d, 0x013a003e,
	0x073d0c26, 0x0b2d0f27, 0x0b2a0d2c, 0x102d0c29, 0x0a311e22, 0x122a0a37,
	0x1133112e, 0x00591aed, 0x16ef1aef, 0x1ee71cec, 0x21e925e5, 0x21e928e4,
	0x26ef21f5, 0x28f129fa, 0x26012911, 0x1efa1b03, 0x1a1625f0, 0x23fc26f8,
	0x26fd2503, 0x26052a00, 0x23102716, 0x0e301b25, 0x153c0c44, 0x0261fd47,
	0xfa2afb32, 0xfd36fe3e, 0x003a013f, 0xfe48ff4a, 0xf75bfb43, 0xfb1bfd27,
	0xfe2c002e, 0xf040f844, 0xf64efa4d, 0xf656f45c, 0xf137f63c, 0xfa3efc41,
	0xf449f84c, 0xf950f758, 0xef6ef561, 0xec54f54f, 0xfa49fc4a, 0xf356f360,
	0xf561ed75, 0xf84efb21, 0xfc30fe35, 0xfd3ef347, 0xf64ff456, 0xf35af261,
	0x0000fa5d, 0xfa54f84f, 0x0042ff47, 0x003efe3c, 0xfe3bfb4b, 0xfd3efc3a,
	0xf742ff4f, 0x00470344, 0x0a2cf93e, 0x0f240e28, 0x101b0c1d, 0x012c1424,
	0x1220052a, 0x01300a3e, 0x112e0940, 0xf468f561, 0xf060f958, 0xf855f955,
	0xf755f358, 0x0442fd4d, 0xfd4cfa4c, 0x0a3aff4c, 0xff53f963, 0xf25f025f,
	0x004cfb4a, 0x0046f54b, 0x01440041, 0xf249033e, 0x043eff44, 0xf34b0b37,
	0x05400c46, 0x0f060613, 0x07100c0e, 0x120d0d0b, 0x0d0f0f10, 0x0c170d17,
	0x0f140e1a, 0x0e2c1128, 0x112f1811, 0x15151916, 0x1f1b161d, 0x13230e32,
	0x0a39073f, 0xfe4dfc52, 0xfd5e0945, 0xf46d24dd, 0x24de20e6, 0x25e22ce0,
	0x22ee22f1, 0x28f121f9, 0x23fb2100, 0x2602210d, 0x17230d3a, 0x1dfd1a00,
	0x161e1ff9, 0x23f122fd, 0x220324ff, 0x2205200b, 0x2305220c, 0x270b1e1d,
	0x221a1d27, 0x13421f15, 0x1f1f1932, 0xef78ec70, 0xee72f555, 0xf15cf259,
	0xe647f151, 0xf2500044, 0xf246e838, 0xe944e832, 0xf54a17f3, 0x1af328f1,
	0x31f22c03, 0x2d062c22, 0x21361352, 0xfd4bff17, 0x0122012b, 0x0036fe37,
	0x003d0140, 0x0044f75c, 0xf26af361, 0xf15af45a, 0xee58f649, 0xf74ff256,
	0xf649f646, 0xf645fb42, 0xf740fb3a, 0x023b15f6, 0x18f51cf8, 0x1cff1d03,
	0x1d092314, 0x1d240e43, 0x14f10236, 0x034a14f1, 0x0236034a, 0xe47fe968,
	0xfa35ff36, 0x07331721, 0x17021500, 0x01090031, 0xdb760539, 0xf34ef541,
	0x013e0c31, 0xfc491132, 0x1240092b, 0x1d001a43, 0x105a0968, 0xd27fec68,
	0x0143f34e, 0xf541013e, 0xfa56ef5f, 0xfa3d092d, 0xfd45fa51, 0xf5600637,
	0x0743fb56, 0x0258003a, 0xfd4cf65e, 0x05360445, 0xfd510058, 0xf943fb4a,
	0xfc4afb50, 0xf948013a, 0x0029003f, 0x003f003f, 0xf7530456, 0x0061f948,
	0x0d29033e, 0x002dfc4e, 0xfd60e57e, 0xe462e765, 0xe943e452, 0xec5ef053,
	0xea6eeb5b, 0xee66f35d, 0xe37ff95c, 0xfb59f960, 0xf36cfd2e, 0xff41ff39,
	0xf75dfd4a, 0xf75cf857, 0xe97e0536, 0x063c063b, 0x0645ff30, 0x0044fc45,
	0xf858fe55, 0xfa4eff4b, 0xf94d0236, 0x0532fd44, 0x0132062a, 0xfc51013f,
	0xfc460043, 0x0239fe4c, 0x0b230440, 0x013d0b23, 0x12190c18, 0x0d1d0d24,
	0xf65df949, 0xfe490d2e, 0x0931f964, 0x09350235, 0x0535fe3d, 0x00380038,
	0xf33ffb3c, 0xff3e0439, 0xfa450439, 0x0e270433, 0x0d440340, 0x013d093f,
	0x07321027, 0x052c0434, 0x0b30fb3c, 0xff3b003b, 0x1621052c, 0x0e2bff4e,
	0x003c0945, 0x0b1c0228, 0x032c0031, 0x002e022c, 0x0233002f, 0x0427023e,
	0x062e0036, 0x0336023a, 0x043f0633, 0x06390735, 0x06340637, 0x0b2d0e24,
	0x0835ff52, 0x0737fd4e, 0x0f2e161f, 0xff541907, 0x1ef91c03, 0x1c042000,
	0x22ff1e06, 0x1e062009, 0x1f131a1b, 0x1a1e2514, 0x1c221146, 0x0143053b,
	0x0943101e, 0x12201223, 0x161d181f, 0x1726122b, 0x14290b3f, 0x093b0940,
	0xff5efe59, 0xf76cfa4c, 0xfe2c002d, 0x0034fd40, 0xfe3bfc46, 0xfc4bf852,
	0xef66f74d, 0x0318002a, 0x00300037, 0xfa3bf947, 0xf453f557, 0xe277013a,
	0xfd1dff24, 0x0126022b, 0xfa37003a, 0x0040fd4a, 0xf65a0046, 0xfc1d051f,
	0x072a013b, 0xfe3afd48, 0xfd51f561, 0x003a0805, 0x0a0e0e12, 0x0d1b0228,
	0x003afd46, 0xfa4ff855, 0x0000f36a, 0xf06af657, 0xeb72ee6e, 0xf262ea6e,
	0xeb6aee67, 0xeb6be96c, 0xe670f660, 0xf45ffb5b, 0xf75dea5e, 0xfb560943,
	0xfc50f655, 0xff46073c, 0x093a053d, 0x0c320f32, 0x12311136, 0x0a29072e,
	0xff330731, 0x08340929, 0x062f0237, 0x0d290a2c, 0x06320535, 0x0d31043f,
	0x0640fe45, 0xfe3b0646, 0x0a2c091f, 0x0c2b0335, 0x0e220a26, 0xfd340d28,
	0x1120072c, 0x07260d32, 0x0a391a2b, 0x0e0b0b0e, 0x090b120b, 0x150917fe,
	0x20f120f1, 0x22eb27e9, 0x2adf29e1, 0x2ee426f4, 0x151d2de8, 0x35d330e6,
	0x41d52bed, 0x27f61e09, 0x121a141b, 0x0039f252, 0xfb4bed61, 0xdd7d1b00,
	0x1c001ffc, 0x1b062208, 0x1e0a1816, 0x21131620, 0x1a1f1529, 0x1a2c172f,
	0x10410e47, 0x083c063f, 0x11411518, 0x17141a17, 0x1b201c17, 0x1c181728,
	0x18201c1d, 0x172a1339, 0x1635163d, 0x0b560c28, 0x0b330e3b, 0xfc4ff947,
	0xfb45f746, 0xf842f644, 0xed49f445, 0xf046f143, 0xec3eed46, 0xf042ea41,
	0xec3f09fe, 0x1af721f7, 0x27f929fe, 0x2d033109, 0x2d1b243b, 0xfa42f923,
	0xf92af82d, 0xfb30f438, 0xfa3cfb3e, 0xf842f84c, 0xfb55fa51, 0xf64df951,
	0xef50ee49, 0xfc4af653, 0xf747f743, 0xff3df842, 0xf242003b, 0x023b15f3,
	0x21f227f9, 0x2efe3302, 0x3c063d11, 0x37222a3e, 0x14f10236, 0x034a14f1,
	0x0236034a, 0xe47fe968, 0xfa35ff36, 0x07331619, 0x22001000, 0xfe090429,
	0xe3760241, 0xfa47f34f, 0x05340932, 0xfd460a36, 0x1a221316, 0x28003902,
	0x29241a45, 0xd37ff165, 0xfc4cfa47, 0xf34f0534, 0x0645f35a, 0x0034082b,
	0xfe45fb52, 0xf660023b, 0x024bfd57, 0xfd640138, 0xfd4afa55, 0x003bfd51,
	0xf956fb5f, 0xff42ff4d, 0x0146fe56, 0xfb48003d, 0x0029003f, 0x003f003f,
	0xf7530456, 0x0061f948, 0x0d29033e, 0x0d0f0733, 0x0250d97f, 0xee5bef60,
	0xe651dd62, 0xe866e961, 0xe577e863, 0xeb6eee66, 0xdc7f0050, 0xfb59f95e,
	0xfc5c0027, 0x0041f154, 0xdd7ffe49, 0xf468f75b, 0xe17f0337, 0x07380737,
	0x083dfd35, 0x0044f94a, 0xf758f367, 0xf35bf759, 0xf25cf84c, 0xf457e96e,
	0xe869f64e, 0xec70ef63, 0xb27fba7f, 0xce7fd27f, 0xfc42fb4e, 0xfc47f848,
	0x023bff37, 0xf946fa4b, 0xf859de77, 0xfd4b2014, 0x1e16d47f, 0x0036fb3d,
	0x003aff3c, 0xfd3df843, 0xe754f24a, 0xfb410534, 0x0239003d, 0xf745f546,
	0x1237fc47, 0x003a073d, 0x09291219, 0x0920052b, 0x092f002c, 0x0033022e,
	0x1326fc42, 0x0f260c2a, 0x09220059, 0x042d0a1c, 0x0a1f21f5, 0x34d5120f,
	0x1c0023ea, 0x26e72200, 0x27ee20f4, 0x66a20000, 0x38f121fc, 0x1d0a25fb,
	0x33e327f7, 0x34de45c6, 0x43c12cfb, 0x200737e3, 0x20010000, 0x1b2421e7,
	0x22e224e4, 0x26e426e5, 0x22ee23f0, 0x22f220f8, 0x25fa2300, 0x1e0a1c12,
	0x1a191d29, 0x004b0248, 0x084d0e23, 0x121f1123, 0x151e112d, 0x142a122d,
	0x1b1a1036, 0x07421038, 0x0b490a43, 0xf674e970, 0xf147f93d, 0x0035fb42,
	0xf54df750, 0xf754f657, 0xde7feb65, 0xfd27fb35, 0xf93df54b, 0xf14def5b,
	0xe76be76f, 0xe47af54c, 0xf62cf634, 0xf639f73a, 0xf048f945, 0xfc45fb4a,
	0xf7560242, 0xf7220120, 0x0b1f0534, 0xfe37fe43, 0x0049f859, 0x03340704,
	0x0a081108, 0x10130325, 0xff3dfb49, 0xff46fc4e, 0x0000eb7e, 0xe97cec6e,
	0xe67ee77c, 0xef69e579, 0xe575ef66, 0xe675e574, 0xdf7af65f, 0xf264f85f,
	0xef6fe472, 0xfa59fe50, 0xfc52f755, 0xf851ff48, 0x05400143, 0x09380045,
	0x01450745, 0xf945fa43, 0xf04dfe40, 0x023dfa43, 0xfd400239, 0xfd41fd42,
	0x003e0933, 0xff42fe47, 0xfe4bff46, 0xf7480e3c, 0x1025002f, 0x12230b25,
	0x0c290a29, 0x02300c29, 0x0d29003b, 0x03321328, 0x03421232, 0x13fa12fa,
	0x0e001af4, 0x1ff021e7, 0x21ea25e4, 0x27e22ae2, 0x2fd62ddc, 0x31de29ef,
	0x200945b9, 0x3fc142c0, 0x4db636d9, 0x34dd29f6, 0x240028ff, 0x1e0e1c1a,
	0x17250c37, 0x0b4125df, 0x27dc28db, 0x26e22edf, 0x2ae228e8, 0x31e326f4,
	0x28f626fd, 0x2efb1f14, 0x1d1e192c, 0x0c300b31, 0x1a2d1616, 0x17161b15,
	0x21141a1c, 0x1e181b22, 0x122a1927, 0x12320c46, 0x15360e47, 0x0b531920,
	0x15311536, 0xfb55fa51, 0xf64df951, 0xef50ee49, 0xfc4af653, 0xf747f743,
	0xff3df842, 0xf242003b, 0x023b11f6, 0x20f32af7, 0x31fb3500, 0x4003440a,
	0x421b2f39, 0xfb470018, 0xff24fe2a, 0xfe34f739, 0xfa3ffc41, 0xfc43f952,
	0xfd51fd4c, 0xf948fa4e, 0xf448f244, 0xfd46fa4c, 0xfb42fb3e, 0x0039fc3d,
	0xf73c0136, 0x023a11f6, 0x20f32af7, 0x31fb3500, 0x4003440a, 0x421b2f39,
	0x14f10236, 0x034a14f1, 0x0236034a, 0xe47fe968, 0xfa35ff36, 0x07331d10,
	0x19000e00, 0xf633fd3e, 0xe5631a10, 0xfc55e866, 0x05390639, 0xef490e39,
	0x1428140a, 0x1d003600, 0x252a0c61, 0xe07fea75, 0xfe4afc55, 0xe8660539,
	0xfa5df258, 0xfa2c0437, 0xf559f167, 0xeb741339, 0x143a0454, 0x0660013f,
	0xfb55f36a, 0x053f064b, 0xfd5aff65, 0x0337fc4f, 0xfe4bf461, 0xf932013c,
	0x0029003f, 0x003f003f, 0xf7530456, 0x0061f948, 0x0d29033e, 0x0722f758,
	0xec7fdc7f, 0xef5bf25f, 0xe754e756, 0xf459ef5b, 0xe17ff24c, 0xee67f35a,
	0xdb7f0b50, 0x054c0254, 0x054efa37, 0x043df253, 0xdb7ffb4f, 0xf568f55b,
	0xe27f0041, 0xfe4f0048, 0xfc5cfa38, 0x0344f847, 0xf362fc56, 0xf458fb52,
	0xfd48fc43, 0xf848f059, 0xf745ff3b, 0x05420439, 0xfc47fe47, 0x023aff4a,
	0xfc2cff45, 0x003ef933, 0xfc2ffa2a, 0xfd29fa35, 0x084cf74e, 0xf5530934,
	0x0043fb5a, 0x0143f148, 0xfb4bf850, 0xeb53eb40, 0xf31fe740, 0xe35e094b,
	0x113ff84a, 0xfb23fe1b, 0x0d5b0341, 0xf945084d, 0xf642033e, 0xfd44ec51,
	0x001e0107, 0xfd17eb4a, 0x1042e97c, 0x11252cee, 0x32deea7f, 0x0427002a,
	0x07220b1d, 0x081f0625, 0x072a0328, 0x08210d2b, 0x0d24042f, 0x0337023a,
	0x063c082c, 0x0b2c0e2a, 0x07300438, 0x04340d25, 0x0931133a, 0x0a300c2d,
	0x00451421, 0x083f23ee, 0x21e71cfd, 0x180a1b00, 0x22f234d4, 0x27e81311,
	0x1f19241d, 0x1821220f, 0x1e141649, 0x1422131f, 0x1b2c1310, 0x0f240f24,
	0x151c1915, 0x1e141f0c, 0x1b10182a, 0x005d0e38, 0x0f391a26, 0xe87fe873,
	0xea52f73e, 0x0035003b, 0xf255f359, 0xf35ef55c, 0xe37feb64, 0xf239f443,
	0xf547f64d, 0xeb55f058, 0xe968f162, 0xdb7ff652, 0xf830f83d, 0xf842f946,
	0xf24bf64f, 0xf753f45c, 0xee6cfc4f, 0xea45f04b, 0xfe3a013a, 0xf34ef753,
	0xfc51f363, 0xf351fa26, 0xf33efa3a, 0xfe3bf049, 0xf64cf356, 0xf753f657,
	0x0000ea7f, 0xe77fe778, 0xe57fed72, 0xe975e776, 0xe675e871, 0xe476e178,
	0xdb7cf65e, 0xf166f663, 0xf36ace7f, 0xfb5c1139, 0xfb56f35e, 0xf45bfe4d,
	0x0047ff49, 0x0440f951, 0x05400f39, 0x01430044, 0xf6430144, 0x004d0240,
	0x0044fb4e, 0x0737053b, 0x02410e36, 0x0f2c053c, 0x0246fe4c, 0xee560c46,
	0x0540f446, 0x0b370538, 0x00450241, 0xfa4a0536, 0x0736fa4c, 0xf552fe4d,
	0xfe4d192a, 0x11f310f7, 0x11f41beb, 0x25e229d8, 0x2ad730d1, 0x27e02ed8,
	0x34cd2ed7, 0x34d92bed, 0x200b3dc9, 0x38d23ece, 0x51bd2dec, 0x23fe1c0f,
	0x22012701, 0x1e111426, 0x122d0f36, 0x004f24f0, 0x25f225ef, 0x2001220f,
	0x1d0f1819, 0x22161f10, 0x23121f1c, 0x2129241c, 0x1b2f153e, 0x121f131a,
	0x24181817, 0x1b10181e, 0x1f1d1629, 0x162a103c, 0x0f340e3c, 0x034ef07b,
	0x15351638, 0x193d1521, 0x1332113d, 0xfd4ef84a, 0xf748f648, 0xee4bf447,
	0xf53ffb46, 0xef4bf248, 0xf043f835, 0xf23bf734, 0xf54409fe, 0x1ef61ffc,
	0x21ff2107, 0x1f0c2517, 0x1f261440, 0xf747f925, 0xf82cf531, 0xf638f43b,
	0xf83ff743, 0xfa44f64f, 0xfd4ef84a, 0xf748f648, 0xee4bf447, 0xf53ffb46,
	0xef4bf248, 0xf043f835, 0xf23bf734, 0xf54409fe, 0x1ef61ffc, 0x21ff2107,
	0x1f0c2517, 0x1f261440
};

static void
assemble_scaling_list(struct hantro_ctx *ctx)
{
	const struct hantro_h264_dec_ctrls *ctrls = &ctx->h264_dec.ctrls;
	const struct v4l2_ctrl_h264_scaling_matrix *scaling = ctrls->scaling;
	const struct v4l2_ctrl_h264_pps *pps = ctrls->pps;
	const size_t num_list_4x4 = ARRAY_SIZE(scaling->scaling_list_4x4);
	const size_t list_len_4x4 = ARRAY_SIZE(scaling->scaling_list_4x4[0]);
	const size_t list_len_8x8 = ARRAY_SIZE(scaling->scaling_list_8x8[0]);
	struct hantro_h264_dec_priv_tbl *tbl = ctx->h264_dec.priv.cpu;
	u32 *dst = (u32 *)tbl->scaling_list;
	const u32 *src;
	int i, j;

	if (!(pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT))
		return;

	for (i = 0; i < num_list_4x4; i++) {
		src = (u32 *)&scaling->scaling_list_4x4[i];
		for (j = 0; j < list_len_4x4 / 4; j++)
			*dst++ = swab32(src[j]);
	}

	/* Only Intra/Inter Y lists */
	for (i = 0; i < 2; i++) {
		src = (u32 *)&scaling->scaling_list_8x8[i];
		for (j = 0; j < list_len_8x8 / 4; j++)
			*dst++ = swab32(src[j]);
	}
}

static void prepare_table(struct hantro_ctx *ctx)
{
	const struct hantro_h264_dec_ctrls *ctrls = &ctx->h264_dec.ctrls;
	const struct v4l2_ctrl_h264_decode_params *dec_param = ctrls->decode;
	const struct v4l2_ctrl_h264_sps *sps = ctrls->sps;
	struct hantro_h264_dec_priv_tbl *tbl = ctx->h264_dec.priv.cpu;
	const struct v4l2_h264_dpb_entry *dpb = ctx->h264_dec.dpb;
	u32 dpb_longterm = 0;
	u32 dpb_valid = 0;
	int i;

	for (i = 0; i < HANTRO_H264_DPB_SIZE; ++i) {
		tbl->poc[i * 2] = dpb[i].top_field_order_cnt;
		tbl->poc[i * 2 + 1] = dpb[i].bottom_field_order_cnt;

		if (!(dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID))
			continue;

		/*
		 * Set up bit maps of valid and long term DPBs.
		 * NOTE: The bits are reversed, i.e. MSb is DPB 0. For frame
		 * decoding, bit 31 to 15 are used, while for field decoding,
		 * all bits are used, with bit 31 being a top field, 30 a bottom
		 * field and so on.
		 */
		if (dec_param->flags & V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC) {
			if (dpb[i].fields & V4L2_H264_TOP_FIELD_REF)
				dpb_valid |= REF_BIT(i * 2);

			if (dpb[i].fields & V4L2_H264_BOTTOM_FIELD_REF)
				dpb_valid |= REF_BIT(i * 2 + 1);

			if (dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM) {
				dpb_longterm |= REF_BIT(i * 2);
				dpb_longterm |= REF_BIT(i * 2 + 1);
			}
		} else {
			dpb_valid |= REF_BIT(i);

			if (dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM)
				dpb_longterm |= REF_BIT(i);
		}
	}
	ctx->h264_dec.dpb_valid = dpb_valid;
	ctx->h264_dec.dpb_longterm = dpb_longterm;

	if ((dec_param->flags & V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC) ||
	    !(sps->flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD)) {
		tbl->poc[32] = ctx->h264_dec.cur_poc;
		tbl->poc[33] = 0;
	} else {
		tbl->poc[32] = dec_param->top_field_order_cnt;
		tbl->poc[33] = dec_param->bottom_field_order_cnt;
	}

	assemble_scaling_list(ctx);
}

static bool dpb_entry_match(const struct v4l2_h264_dpb_entry *a,
			    const struct v4l2_h264_dpb_entry *b)
{
	return a->reference_ts == b->reference_ts;
}

static void update_dpb(struct hantro_ctx *ctx)
{
	const struct v4l2_ctrl_h264_decode_params *dec_param;
	DECLARE_BITMAP(new, ARRAY_SIZE(dec_param->dpb)) = { 0, };
	DECLARE_BITMAP(used, ARRAY_SIZE(dec_param->dpb)) = { 0, };
	unsigned int i, j;

	dec_param = ctx->h264_dec.ctrls.decode;

	/* Disable all entries by default. */
	for (i = 0; i < ARRAY_SIZE(ctx->h264_dec.dpb); i++)
		ctx->h264_dec.dpb[i].flags = 0;

	/* Try to match new DPB entries with existing ones by their POCs. */
	for (i = 0; i < ARRAY_SIZE(dec_param->dpb); i++) {
		const struct v4l2_h264_dpb_entry *ndpb = &dec_param->dpb[i];

		if (!(ndpb->flags & V4L2_H264_DPB_ENTRY_FLAG_VALID))
			continue;

		/*
		 * To cut off some comparisons, iterate only on target DPB
		 * entries which are not used yet.
		 */
		for_each_clear_bit(j, used, ARRAY_SIZE(ctx->h264_dec.dpb)) {
			struct v4l2_h264_dpb_entry *cdpb;

			cdpb = &ctx->h264_dec.dpb[j];
			if (!dpb_entry_match(cdpb, ndpb))
				continue;

			*cdpb = *ndpb;
			__set_bit(j, used);
			break;
		}

		if (j == ARRAY_SIZE(ctx->h264_dec.dpb))
			__set_bit(i, new);
	}

	/* For entries that could not be matched, use remaining free slots. */
	for_each_set_bit(i, new, ARRAY_SIZE(dec_param->dpb)) {
		const struct v4l2_h264_dpb_entry *ndpb = &dec_param->dpb[i];
		struct v4l2_h264_dpb_entry *cdpb;

		/*
		 * Both arrays are of the same sizes, so there is no way
		 * we can end up with no space in target array, unless
		 * something is buggy.
		 */
		j = find_first_zero_bit(used, ARRAY_SIZE(ctx->h264_dec.dpb));
		if (WARN_ON(j >= ARRAY_SIZE(ctx->h264_dec.dpb)))
			return;

		cdpb = &ctx->h264_dec.dpb[j];
		*cdpb = *ndpb;
		__set_bit(j, used);
	}
}

dma_addr_t hantro_h264_get_ref_buf(struct hantro_ctx *ctx,
				   unsigned int dpb_idx)
{
	struct v4l2_h264_dpb_entry *dpb = ctx->h264_dec.dpb;
	dma_addr_t dma_addr = 0;
	s32 cur_poc = ctx->h264_dec.cur_poc;
	u32 flags;

	if (dpb[dpb_idx].flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE)
		dma_addr = hantro_get_ref(ctx, dpb[dpb_idx].reference_ts);

	if (!dma_addr) {
		struct vb2_v4l2_buffer *dst_buf;
		struct vb2_buffer *buf;

		/*
		 * If a DPB entry is unused or invalid, address of current
		 * destination buffer is returned.
		 */
		dst_buf = hantro_get_dst_buf(ctx);
		buf = &dst_buf->vb2_buf;
		dma_addr = hantro_get_dec_buf_addr(ctx, buf);
	}

	flags = dpb[dpb_idx].flags & V4L2_H264_DPB_ENTRY_FLAG_FIELD ? 0x2 : 0;
	flags |= abs(dpb[dpb_idx].top_field_order_cnt - cur_poc) <
		 abs(dpb[dpb_idx].bottom_field_order_cnt - cur_poc) ?
		 0x1 : 0;

	return dma_addr | flags;
}

u16 hantro_h264_get_ref_nbr(struct hantro_ctx *ctx, unsigned int dpb_idx)
{
	const struct v4l2_h264_dpb_entry *dpb = &ctx->h264_dec.dpb[dpb_idx];

	if (!(dpb->flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE))
		return 0;
	return dpb->frame_num;
}

/*
 * Removes all references with the same parity as the current picture from the
 * reference list. The remaining list will have references with the opposite
 * parity. This is effectively a deduplication of references since each buffer
 * stores two fields. For this reason, each buffer is found twice in the
 * reference list.
 *
 * This technique has been chosen through trial and error. This simple approach
 * resulted in the highest conformance score. Note that this method may suffer
 * worse quality in the case an opposite reference frame has been lost. If this
 * becomes a problem in the future, it should be possible to add a preprocessing
 * to identify un-paired fields and avoid removing them.
 */
static void deduplicate_reflist(struct v4l2_h264_reflist_builder *b,
				struct v4l2_h264_reference *reflist)
{
	int write_idx = 0;
	int i;

	if (b->cur_pic_fields == V4L2_H264_FRAME_REF) {
		write_idx = b->num_valid;
		goto done;
	}

	for (i = 0; i < b->num_valid; i++) {
		if (!(b->cur_pic_fields == reflist[i].fields)) {
			reflist[write_idx++] = reflist[i];
			continue;
		}
	}

done:
	/* Should not happen unless we have a bug in the reflist builder. */
	if (WARN_ON(write_idx > 16))
		write_idx = 16;

	/* Clear the remaining, some streams fails otherwise */
	for (; write_idx < 16; write_idx++)
		reflist[write_idx].index = 15;
}

int hantro_h264_dec_prepare_run(struct hantro_ctx *ctx)
{
	struct hantro_h264_dec_hw_ctx *h264_ctx = &ctx->h264_dec;
	struct hantro_h264_dec_ctrls *ctrls = &h264_ctx->ctrls;
	struct v4l2_h264_reflist_builder reflist_builder;

	hantro_start_prepare_run(ctx);

	ctrls->scaling =
		hantro_get_ctrl(ctx, V4L2_CID_STATELESS_H264_SCALING_MATRIX);
	if (WARN_ON(!ctrls->scaling))
		return -EINVAL;

	ctrls->decode =
		hantro_get_ctrl(ctx, V4L2_CID_STATELESS_H264_DECODE_PARAMS);
	if (WARN_ON(!ctrls->decode))
		return -EINVAL;

	ctrls->sps =
		hantro_get_ctrl(ctx, V4L2_CID_STATELESS_H264_SPS);
	if (WARN_ON(!ctrls->sps))
		return -EINVAL;

	ctrls->pps =
		hantro_get_ctrl(ctx, V4L2_CID_STATELESS_H264_PPS);
	if (WARN_ON(!ctrls->pps))
		return -EINVAL;

	/* Update the DPB with new refs. */
	update_dpb(ctx);

	/* Build the P/B{0,1} ref lists. */
	v4l2_h264_init_reflist_builder(&reflist_builder, ctrls->decode,
				       ctrls->sps, ctx->h264_dec.dpb);
	h264_ctx->cur_poc = reflist_builder.cur_pic_order_count;

	/* Prepare data in memory. */
	prepare_table(ctx);

	v4l2_h264_build_p_ref_list(&reflist_builder, h264_ctx->reflists.p);
	v4l2_h264_build_b_ref_lists(&reflist_builder, h264_ctx->reflists.b0,
				    h264_ctx->reflists.b1);

	/*
	 * Reduce ref lists to at most 16 entries, Hantro hardware will deduce
	 * the actual picture lists in field through the dpb_valid,
	 * dpb_longterm bitmap along with the current frame parity.
	 */
	if (reflist_builder.cur_pic_fields != V4L2_H264_FRAME_REF) {
		deduplicate_reflist(&reflist_builder, h264_ctx->reflists.p);
		deduplicate_reflist(&reflist_builder, h264_ctx->reflists.b0);
		deduplicate_reflist(&reflist_builder, h264_ctx->reflists.b1);
	}

	return 0;
}

void hantro_h264_dec_exit(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct hantro_h264_dec_hw_ctx *h264_dec = &ctx->h264_dec;
	struct hantro_aux_buf *priv = &h264_dec->priv;

	dma_free_coherent(vpu->dev, priv->size, priv->cpu, priv->dma);
}

int hantro_h264_dec_init(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct hantro_h264_dec_hw_ctx *h264_dec = &ctx->h264_dec;
	struct hantro_aux_buf *priv = &h264_dec->priv;
	struct hantro_h264_dec_priv_tbl *tbl;

	priv->cpu = dma_alloc_coherent(vpu->dev, sizeof(*tbl), &priv->dma,
				       GFP_KERNEL);
	if (!priv->cpu)
		return -ENOMEM;

	priv->size = sizeof(*tbl);
	tbl = priv->cpu;
	memcpy(tbl->cabac_table, h264_cabac_table, sizeof(tbl->cabac_table));

	return 0;
}
