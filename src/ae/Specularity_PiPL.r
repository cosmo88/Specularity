#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
	#include "AE_General.r"
#endif

resource 'PiPL' (16000) {
	{
		Kind { AEEffect },
		Name { "Specularity" },
		Category { "Stylize" },

#ifdef AE_OS_WIN
    #if defined(AE_PROC_INTELx64)
		CodeWin64X86 {"EffectMain"},
    #elif defined(AE_PROC_ARM64)
		CodeWinARM64 {"EffectMain"},
    #endif
#elif defined(AE_OS_MAC)
		CodeMacIntel64 {"EffectMain"},
		CodeMacARM64 {"EffectMain"},
#endif

		AE_PiPL_Version { 2, 0 },
		AE_Effect_Spec_Version { PF_PLUG_IN_VERSION, PF_PLUG_IN_SUBVERS },
		AE_Effect_Version { 2097153 },   /* PF_VERSION(4,0,0,PF_Stage_DEVELOP,1) = 0x200001 */
		AE_Effect_Info_Flags { 0 },
		AE_Effect_Global_OutFlags  { 0x2000440 },
		AE_Effect_Global_OutFlags_2 { 0x8201400 },
		AE_Effect_Match_Name { "com.cosmo88.ae.Specularity" },
		AE_Reserved_Info { 8 },
		AE_Effect_Support_URL { "https://github.com/cosmo88/Specularity" }
	}
};
