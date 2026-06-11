/*****************************************************************************/
/*                                                                           */
/*        --- CAEN Engineering Srl - Computing Systems Division ---          */
/*                                                                           */
/*   MAINWRAPP.C                                                             */
/*                                                                           */
/*   June      2000:  Rel. 1.0                                               */
/*   Frebruary 2001:  Rel. 1.1                                               */
/*   September 2002:  Rel. 2.6                                               */
/*   November  2002:  Rel. 2.7                                               */
/*																	         */
/*****************************************************************************/
#include <signal.h>
#ifdef UNIX
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include "MainWrapp.h"
#include "console.h"
#include "CAENHVWrapper.h"

#define MAX_CMD_LEN        (80)

/* Common error/capacity macros (no behavior change; only deduplicate checks) */
#define CHECK_ALLOC_RETURN(ptr) \
	do { if(!(ptr)) { fprintf(stderr, "Out of memory\n"); return 3; } } while(0)

#define CHECK_ALLOC_SETEXIT(ptr, exitVar) \
	do { if(!(ptr)) { fprintf(stderr, "Out of memory\n"); if((exitVar)==0) (exitVar)=3; } } while(0)

/* =========================
   Default CLI configuration
   ========================= */
#define DEFAULT_SYSTEM	SY4527
#define DEFAULT_LINK	LINKTYPE_TCPIP
#define DEFAULT_HOST	"192.168.100.2"
#define DEFAULT_USER	"admin"
#define DEFAULT_PASS	"admin"
#define DEFAULT_SLOT	3

/* ----------------------------------------
   Channels to exclude when using '--ch all'
   Edit the list below to skip channels.
   Example: { 3, 7, 15 }
   ---------------------------------------- */
#define EXCLUDED_CH_COUNT 0
static const unsigned short EXCLUDED_CH[EXCLUDED_CH_COUNT] = { };

static int is_channel_excluded(unsigned short ch)
{
	for(int i = 0; i < EXCLUDED_CH_COUNT; i++)
		if(EXCLUDED_CH[i] == ch)
			return 1;
	return 0;
}

/* Default config file paths (first existing one will be used) */
#define DEFAULT_CONFIG_PATH "../config/config.txt"

/* strict token parsers to validate numeric fields (avoid treating headers as data) */
static int parse_ushort_token(const char *s, unsigned short *out)
{
	char *endp = NULL;
	if(s == NULL || *s == '\0') return 0;
	unsigned long v = strtoul(s, &endp, 10);
	if(endp == s || *endp != '\0') return 0;
	if(v > 0xFFFF) return 0;
	*out = (unsigned short)v;
	return 1;
}

static int parse_float_token(const char *s, float *out)
{
	char *endp = NULL;
	if(s == NULL || *s == '\0') return 0;
	double v = strtod(s, &endp);
	if(endp == s || *endp != '\0') return 0;
	*out = (float)v;
	return 1;
}

/* ----------------------------------------
   Config file loader
   Format (whitespace or commas as separators):
     ch#   chName   V0Set   I0Set   [SVMax]
   chName is used for display purposes only (not written back).
   ---------------------------------------- */
static int load_config_file(const char *path,
	unsigned short **outChList, int *outCount,
	float **outV0List, float **outI0List,
	float **outSVMaxList,
	float **outRampUpList, float **outRampDownList,
	char ***outNameList)
{
	FILE *fp = fopen(path, "r");
	if(!fp) return -1;

	unsigned short *chVec = NULL;
	float *v0Vec = NULL;
	float *i0Vec = NULL;
	float *svVec = NULL;
	float *ruVec = NULL;
	float *rdVec = NULL;
	char **nameVec = NULL;
	int cap = 0;
	int len = 0;
	char line[1024];

	while(fgets(line, sizeof(line), fp)) {
		/* skip empty/comment lines */
		char *p = line;
		while(*p == ' ' || *p == '\t') p++;
		if(*p == '\0' || *p == '\n' || *p == '#')
			continue;

		/* tokenize */
		const char *delims = " ,\t\r\n";
		char *tok = strtok(line, delims);
		if(!tok) continue;
		unsigned short ch;
		if(!parse_ushort_token(tok, &ch))
			continue; /* header or invalid first token: skip line */

		/* second token: chName (kept for display) */
		tok = strtok(NULL, delims);
		if(!tok) continue;
		const char *nameTok = tok;

		/* third: V0Set */
		tok = strtok(NULL, delims);
		if(!tok) continue;
		float v0;
		if(!parse_float_token(tok, &v0))
			continue;

		/* fourth: I0Set */
		tok = strtok(NULL, delims);
		if(!tok) continue;
		float i0;
		if(!parse_float_token(tok, &i0))
			continue;

		/* fifth: optional SVMax */
		tok = strtok(NULL, delims);
		float svmax = 0.0f;
		int hasSV = 0;
		if(tok && parse_float_token(tok, &svmax)) {
			hasSV = 1;
			tok = strtok(NULL, delims);
		}

		/* sixth: optional RampUp */
		float rampup = 0.0f;
		int hasRU = 0;
		if(tok && parse_float_token(tok, &rampup)) {
			hasRU = 1;
			tok = strtok(NULL, delims);
		}

		/* seventh: optional RampDown */
		float rampdown = 0.0f;
		int hasRD = 0;
		if(tok && parse_float_token(tok, &rampdown)) {
			hasRD = 1;
		}

		if(is_channel_excluded((unsigned short)ch))
			continue;

		if(len >= cap) {
			int ncap = (cap == 0 ? 16 : cap * 2);
			unsigned short *nch = (unsigned short*)realloc(chVec, sizeof(unsigned short) * (size_t)ncap);
			float *nv0 = (float*)realloc(v0Vec, sizeof(float) * (size_t)ncap);
			float *ni0 = (float*)realloc(i0Vec, sizeof(float) * (size_t)ncap);
			float *nsv = (float*)realloc(svVec, sizeof(float) * (size_t)ncap);
			float *nru = (float*)realloc(ruVec, sizeof(float) * (size_t)ncap);
			float *nrd = (float*)realloc(rdVec, sizeof(float) * (size_t)ncap);
			char **nname = (char**)realloc(nameVec, sizeof(char*) * (size_t)ncap);
			if(!nch || !nv0 || !ni0 || !nsv || !nru || !nrd || !nname) {
				if(nch) chVec = nch;
				if(nv0) v0Vec = nv0;
				if(ni0) i0Vec = ni0;
				if(nsv) svVec = nsv;
				if(nru) ruVec = nru;
				if(nrd) rdVec = nrd;
				if(nname) nameVec = nname;
				fclose(fp);
				free(chVec);
				free(v0Vec);
				free(i0Vec);
				free(svVec);
				free(ruVec);
				free(rdVec);
				if(nameVec) {
					for(int ii = 0; ii < len; ii++) free(nameVec[ii]);
				}
				free(nameVec);
				return -2;
			}
			chVec = nch;
			v0Vec = nv0;
			i0Vec = ni0;
			svVec = nsv;
			ruVec = nru;
			rdVec = nrd;
			nameVec = nname;
			cap = ncap;
		}
		chVec[len] = (unsigned short)ch;
		v0Vec[len] = v0;
		i0Vec[len] = i0;
		svVec[len] = hasSV ? svmax : 0.0f;
		ruVec[len] = hasRU ? rampup : 0.0f;
		rdVec[len] = hasRD ? rampdown : 0.0f;
		/* copy name token */
		size_t nlen = strlen(nameTok);
		nameVec[len] = (char*)malloc(nlen + 1);
		if(nameVec[len]) {
			memcpy(nameVec[len], nameTok, nlen + 1);
		}
		len++;
	}
	fclose(fp);

	if(len == 0) {
		free(chVec);
		free(v0Vec);
		free(i0Vec);
		free(svVec);
		free(ruVec);
		free(rdVec);
		if(nameVec) {
			/* No entries; but ensure no leak */
			free(nameVec);
		}
		return 0;
	}

	*outChList = chVec;
	*outV0List = v0Vec;
	*outI0List = i0Vec;
	if(outSVMaxList) *outSVMaxList = svVec; else free(svVec);
	if(outRampUpList) *outRampUpList = ruVec; else free(ruVec);
	if(outRampDownList) *outRampDownList = rdVec; else free(rdVec);
	if(outNameList) *outNameList = nameVec; else {
		if(nameVec) {
			for(int ii = 0; ii < len; ii++) free(nameVec[ii]);
			free(nameVec);
		}
	}
	*outCount = len;
	return len;
}

static int load_default_config(unsigned short **outChList, int *outCount,
	float **outV0List, float **outI0List, float **outSVMaxList,
	float **outRampUpList, float **outRampDownList, char ***outNameList)
{
	return load_config_file(DEFAULT_CONFIG_PATH, outChList, outCount, outV0List, outI0List, outSVMaxList, outRampUpList, outRampDownList, outNameList);
}

typedef void (*P_FUN)(void);

typedef struct cmds
			{
				char  cmdName[MAX_CMD_LEN]; /* nome del comando */
				P_FUN pFun;
			} CMD;

static CMD function[] = {
		{ "LIBRARYRELEASE", HVLibSwRel },
        { "LOGIN", HVSystemLogin },
        { "LOGOUT", HVSystemLogout },
        { "GETCHNAME", HVGetChName },
        { "SETCHNAME", HVSetChName }, 
		{ "GETCHPARAMPROP", HVGetChParamProp },
        { "GETCHPARAM", HVGetChParam },
        { "SETCHPARAM", HVSetChParam },
     //   { "TSTBDPRES", HVTstBdPres },
		{ "GETBDPARAMPROP", HVGetBdParamProp },
		{ "GETBDPARAM", HVGetBdParam },
		{ "SETBDPARAM", HVSetBdParam },					// Rel. 2.7
     /*   { "GETGRPCOMP", HVGetGrpComp },
        { "ADDCHTOGRP", HVAddChToGrp },
        { "REMCHFROMGRP", HVRemChFromGrp },*/
        { "GETCRATEMAP", HVGetCrateMap },
	/*	{ "GETSYSCOMP", HVGetSysComp },*/
		{ "GETEXECLIST", HVGetExecList }, 
		{ "GETSYSPROP", HVGetSysProp },
		{ "SETSYSPROP", HVSetSysProp },
		{ "EXECOMMAND", HVExecComm },
	/*	{ "CAENETCOMMAND", HVCaenetComm }, */
        { "NOCOMMAND", HVnoFunction }
                 };

static char  alpha[] =
		{
			"abcdefghijklmnopqrstuv"
		};

HV System[MAX_HVPS];
int loop;

/*****************************************************************************/
/*                                                                           */
/*  COMMANDLIST                                                              */
/*                                                                           */
/*  Aut: A. Morachioli                                                       */
/*                                                                           */
/*****************************************************************************/
static void commandList(void)
{
	unsigned short  nOfSys = 0, nOfCmd = 0, pageSys = 0, 
					i, j, page = 0, row, column;
	int				cmd;

	while( strcmp(function[nOfCmd].cmdName, "NOCOMMAND") )
		nOfCmd++;

	while( 1 )
	{
		clrscr();
		con_puts("       --- Demonstration of use of CAEN HV Wrapper Library --- ");
		gotoxy(1, 3);
		for(i=page*20;(i<(page*20+20))&&(strcmp(function[i].cmdName,"NOCOMMAND"));i++)
		{
			row = 3 + (i - page * 20)%10;
			column = ((i - page * 20) > 9 ? 30 : 1);
			gotoxy(column, row);
			con_printf("[%c] %s", alpha[i], function[i].cmdName);
		}

		for( j = pageSys*10; (j<pageSys*10+10)&&(System[j].ID!=-1); j++ )
		{
			row = 3+(j-pageSys*10)%10;
			gotoxy(60,row);
			con_printf("System[%d]: %d", j, System[j].Handle);
		}

		gotoxy(1, 14);
		con_printf("[r] Loop = %s",loop ? "Yes" : "No");

		gotoxy(1, 15);
		con_printf("[x] Exit \n\n");

		switch(cmd = tolower(con_getch()))
		{
			// Handle future next page command
			//	case "next page"
			//		if( nOfCmd > page*20 + 20 )
			//			page++;
			//		else
			//			page = 0;
			//       break;

			// Handle future next system command
			//  case "next system"
			//		while( System[nOfSys].ID != -1 )
            //			nOfSys++;
			//		if( nOfSys > pageSys*10+10 )
            //			pageSys++;
			//		else
            //			pageSys = 0;
			//        break;

	   case 'r':
	     loop = (loop ? 0 : 1);
	    break;

		case 'x':
			quitProgram();
			break;

	    default:
			if( cmd >= 'a' && cmd < 'a' + nOfCmd )
				(*function[cmd-'a'].pFun)();      
	        break;
      }
  }
}

/*****************************************************************************/
/*                                                                           */
/*  MAIN                                                                     */
/*                                                                           */
/*  Aut: A. Morachioli                                                       */
/*                                                                           */
/*****************************************************************************/
/* ---------------------- */
/* Simple CLI integration */
/* ---------------------- */
typedef struct {
	char	name[64];
	char	value[128];
} cli_param_t;

static int str_ieq(const char *a, const char *b) {
	if(a == NULL || b == NULL) return 0;
	while(*a && *b) {
		char ca = (char)tolower((unsigned char)*a++);
		char cb = (char)tolower((unsigned char)*b++);
		if(ca != cb) return 0;
	}
	return *a == '\0' && *b == '\0';
}

static int parse_system_type(const char *s, CAENHV_SYSTEM_TYPE_t *out) {
	if(s == NULL || out == NULL) return -1;
	if(str_ieq(s, "SY1527")) { *out = SY1527; return 0; }
	if(str_ieq(s, "SY2527")) { *out = SY2527; return 0; }
	if(str_ieq(s, "SY4527")) { *out = SY4527; return 0; }
	if(str_ieq(s, "SY5527")) { *out = SY5527; return 0; }
	if(str_ieq(s, "V65XX"))  { *out = V65XX;  return 0; }
	if(str_ieq(s, "N1470"))  { *out = N1470;  return 0; }
	if(str_ieq(s, "V8100"))  { *out = V8100;  return 0; }
	if(str_ieq(s, "N568E"))  { *out = N568E;  return 0; }
	if(str_ieq(s, "DT55XX")) { *out = DT55XX; return 0; }
	if(str_ieq(s, "DT55XXE")){ *out = DT55XXE;return 0; }
	if(str_ieq(s, "SMARTHV")){ *out = SMARTHV;return 0; }
	if(str_ieq(s, "NGPS"))   { *out = NGPS;   return 0; }
	if(str_ieq(s, "N1068"))  { *out = N1068;  return 0; }
	if(str_ieq(s, "N1168"))  { *out = N1168;  return 0; }
	if(str_ieq(s, "R6060"))  { *out = R6060;  return 0; }
	return -1;
}

static int parse_link_type(const char *s, int *outLinkType) {
	if(s == NULL || outLinkType == NULL) return -1;
	if(str_ieq(s, "tcpip"))      { *outLinkType = LINKTYPE_TCPIP;    return 0; }
	if(str_ieq(s, "rs232"))      { *outLinkType = LINKTYPE_RS232;    return 0; }
	if(str_ieq(s, "caenet"))     { *outLinkType = LINKTYPE_CAENET;   return 0; }
	if(str_ieq(s, "usb"))        { *outLinkType = LINKTYPE_USB;      return 0; }
	if(str_ieq(s, "optlink") || str_ieq(s, "optical") || str_ieq(s, "optical_link")) { *outLinkType = LINKTYPE_OPTLINK; return 0; }
	if(str_ieq(s, "usbvcp") || str_ieq(s, "usb_vcp")) { *outLinkType = LINKTYPE_USB_VCP; return 0; }
	if(str_ieq(s, "usb3"))       { *outLinkType = LINKTYPE_USB3;     return 0; }
	if(str_ieq(s, "a4818"))      { *outLinkType = LINKTYPE_A4818;    return 0; }
	return -1;
}

static int is_flag(const char *s) {
	return (s && s[0] == '-' && s[1] == '-');
}

/* Simple whitespace trim helper */
static char *trim_ws(char *s) {
	while(*s == ' ' || *s == '\t') s++;
	char *e = s + strlen(s);
	while(e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
	return s;
}

/* Read connection defaults from channel config file header (before data rows).
   Supported keys: system, link, host, user, pass, slot
   Format: key=value or 'key value', ignores lines starting with '#' or ';'
   Stops when reaches a row starting with 'ch' header or a numeric channel index. */
static int read_conn_from_ch_config(const char *path,
	CAENHV_SYSTEM_TYPE_t *outSystem,
	int *outLinkType,
	char *outHost, size_t outHostLen,
	char *outUser, size_t outUserLen,
	char *outPass, size_t outPassLen,
	int *outSlot)
{
	FILE *fp = fopen(path, "r");
	if(!fp) return 0;
	int mask = 0;
	char line[512];
	while(fgets(line, sizeof(line), fp)) {
		char *p = trim_ws(line);
		if(*p == '\0' || *p == '#' || *p == ';') continue;
		if(!strncasecmp(p, "ch", 2)) break; /* header row */
		/* stop if first token is numeric (channel row) */
		char tok[32] = {0};
		if(sscanf(p, " %31s", tok) == 1) {
			unsigned short tch;
			if(parse_ushort_token(tok, &tch)) break;
		}
		/* parse key=value or key value */
		char key[64] = {0}, val[256] = {0};
		if(sscanf(p, " %63[^=]=%255[^\n]", key, val) != 2) {
			if(sscanf(p, " %63s %255s", key, val) != 2) continue;
		}
		char *k = trim_ws(key), *v = trim_ws(val);
		if(str_ieq(k, "system")) {
			CAENHV_SYSTEM_TYPE_t st;
			if(parse_system_type(v, &st) == 0 && outSystem) { *outSystem = st; mask |= 1; }
		} else if(str_ieq(k, "link")) {
			int lt;
			if(parse_link_type(v, &lt) == 0 && outLinkType) { *outLinkType = lt; mask |= 2; }
		} else if(str_ieq(k, "host")) {
			if(outHost && outHostLen) { snprintf(outHost, outHostLen, "%s", v); mask |= 4; }
		} else if(str_ieq(k, "user")) {
			if(outUser && outUserLen) { snprintf(outUser, outUserLen, "%s", v); mask |= 8; }
		} else if(str_ieq(k, "pass") || str_ieq(k, "password")) {
			if(outPass && outPassLen) { snprintf(outPass, outPassLen, "%s", v); mask |= 16; }
		} else if(str_ieq(k, "slot")) {
			int s = atoi(v);
			if(outSlot && s >= 0) { *outSlot = s; mask |= 32; }
		}
	}
	fclose(fp);
	return mask;
}

/* Map Status bitfield to a human-readable label.
   Fault flags are all shown (no priority among them).
   Channel state (On/Off/Up/Down) shows only the highest-priority one. */
static const char *status_label(uint32_t v) {
	static char buf[256];
	static const struct { uint32_t bit; const char *name; } faults[] = {
		{ 1u << 3,  "Over Current" },
		{ 1u << 4,  "Over Voltage" },
		{ 1u << 9,  "Internal Trip" },
		{ 1u << 6,  "External Trip" },
		{ 1u << 15, "Temperature Error" },
		{ 1u << 14, "Power Failure" },
		{ 1u << 13, "Over Voltage Protection" },
		{ 1u << 5,  "Under Voltage" },
		{ 1u << 7,  "Max Voltage" },
		{ 1u << 8,  "External Disable" },
		{ 1u << 10, "Calibration Error" },
		{ 1u << 11, "Unplugged" },
	};
	/* Priority: faults (all shown) > Up/Down (one) > On/Off (one) */
	buf[0] = '\0';
	for(int i = 0; i < (int)(sizeof(faults)/sizeof(faults[0])); i++) {
		if(v & faults[i].bit) {
			if(buf[0] != '\0') strncat(buf, "+", sizeof(buf) - strlen(buf) - 1);
			strncat(buf, faults[i].name, sizeof(buf) - strlen(buf) - 1);
		}
	}
	if(buf[0]) return buf;
	if     (v & (1u << 1)) return "Up";
	else if(v & (1u << 2)) return "Down";
	else if(v & (1u << 0)) return "On";
	return "Off";
}

/* Append a getter parameter to the list with capacity check */
static int add_get_param(const char *par, const char **arr, int *count, int max) {
	if(*count >= max) {
		fprintf(stderr, "Too many getter parameters\n");
		return -1;
	}
	arr[(*count)++] = par;
	return 0;
}

/* Append a setter parameter (name,value) with capacity check */
static int add_set_param(cli_param_t *params, int *count, int max, const char *name, const char *value) {
	if(*count >= max) {
		fprintf(stderr, "Too many parameters specified\n");
		return -1;
	}
	snprintf(params[*count].name, sizeof(params[*count].name), "%s", name);
	snprintf(params[*count].value, sizeof(params[*count].value), "%s", value ? value : "");
	(*count)++;
	return 0;
}

/* Parameters that support both read and write: allow bare '--Param' to mean '--get Param' */
static int is_readwrite_param(const char *name) {
	if(name == NULL || *name == '\0') return 0;
	if(str_ieq(name, "RUp"))    return 1;
	if(str_ieq(name, "RDWn"))   return 1;
	if(str_ieq(name, "V0Set"))  return 1;
	if(str_ieq(name, "I0Set"))  return 1;
	if(str_ieq(name, "SVMax"))  return 1;
	if(str_ieq(name, "Pw"))     return 1;
	if(str_ieq(name, "Trip"))   return 1;
	return 0;
}

static void print_cli_usage(const char *prog) {
	const char *p = (prog && *prog) ? prog : "HVWrappdemo";
	fprintf(stderr, "Usage (CLI mode): %s --ch 0 1 2 3 --V0Set 650 --Pw On\n", p);
	fprintf(stderr, "       (Setting)  %s --host 192.168.0.1 [--slot 3]\n", p);
	fprintf(stderr, "       (read)     %s --ch 0 1 --IMon\n", p);
	fprintf(stderr, "       (read)     %s --ch 0 1 --VMon\n", p);
	fprintf(stderr, "       (read)     %s --ch 0 1 --Status\n", p);
	fprintf(stderr, "       (read all) %s --ch all --IMon\n", p);
	fprintf(stderr, "       (read all) %s --ch all --VMon\n", p);
	fprintf(stderr, "       (read all) %s --ch all --Status\n", p);
	fprintf(stderr, "\nNotes:\n");
	fprintf(stderr, "- Connection defaults to TCP/IP host " DEFAULT_HOST ". Override with --host.\n");
	fprintf(stderr, "- System defaults to SY4527. Login defaults to admin/admin. Slot has a project default.\n");
	fprintf(stderr, "- You can provide multiple parameter assignments: any --<ParamName> <value> is applied to all channels.\n");
	fprintf(stderr, "- If no arguments are provided, the interactive ncurses demo UI is started.\n");
}

static int run_cli(int argc, char **argv) {
	CAENHV_SYSTEM_TYPE_t sysType = DEFAULT_SYSTEM;
	int linkType = DEFAULT_LINK; /* fixed */
	const char *user = NULL;
	const char *pass = NULL;
	int slot = -1;
	const char *host = DEFAULT_HOST;
	unsigned short *chList = NULL;
	int chCount = 0;
	cli_param_t params[32];
	int paramCount = 0;
	const char *getParams[32];
	int getCount = 0;
	int chAll = 0;
	const char *configPath = NULL;
	int i;
	/* Persistent buffers for config header values and CLI host flag */
	char cfgHost[256] = {0}, cfgUser[64] = {0}, cfgPass[64] = {0};
	int cliHostProvided = 0;

	for(i = 1; i < argc; i++) {
		if(str_ieq(argv[i], "--help")) {
			print_cli_usage(argv[0]);
			return 1;
		} else if(str_ieq(argv[i], "--system") && i+1 < argc) {
			if(parse_system_type(argv[i+1], &sysType) != 0) {
				fprintf(stderr, "Unknown --system '%s'\n", argv[i+1]);
				return 2;
			}
			i++;
		} else if(str_ieq(argv[i], "--user") && i+1 < argc) {
			user = argv[++i];
		} else if(str_ieq(argv[i], "--pass") && i+1 < argc) {
			pass = argv[++i];
		} else if(str_ieq(argv[i], "--host") && i+1 < argc) {
			host = argv[++i];
			cliHostProvided = 1;
		} else if(str_ieq(argv[i], "--slot") && i+1 < argc) {
			slot = atoi(argv[++i]);
		} else if(str_ieq(argv[i], "--config") && i+1 < argc) {
			configPath = argv[++i];
		} else if(str_ieq(argv[i], "--get") && i+1 < argc) {
			if(add_get_param(argv[i+1], getParams, &getCount, (int)(sizeof(getParams)/sizeof(getParams[0]))) != 0)
				return 2;
			i++;
		} else if(str_ieq(argv[i], "--IMon")) {
			if(add_get_param("IMon", getParams, &getCount, (int)(sizeof(getParams)/sizeof(getParams[0]))) != 0)
				return 2;
		} else if(str_ieq(argv[i], "--VMon")) {
			if(add_get_param("VMon", getParams, &getCount, (int)(sizeof(getParams)/sizeof(getParams[0]))) != 0)
				return 2;
		} else if(str_ieq(argv[i], "--Status")) {
			if(add_get_param("Status", getParams, &getCount, (int)(sizeof(getParams)/sizeof(getParams[0]))) != 0)
				return 2;
		} else if(str_ieq(argv[i], "--ch")) {
			int j = i + 1;
			if(j < argc && !is_flag(argv[j]) && str_ieq(argv[j], "all")) {
				chAll = 1;
				i = j; /* consume 'all' */
			} else {
				int start = j;
				int count = 0;
				while(j < argc && !is_flag(argv[j])) { j++; count++; }
				if(count <= 0) {
					fprintf(stderr, "Expected one or more channel indices after --ch\n");
					return 2;
				}
				chList = (unsigned short*)malloc(sizeof(unsigned short) * (size_t)count);
				CHECK_ALLOC_RETURN(chList);
				for(int k = 0; k < count; k++) {
					unsigned short chParsed = 0;
					if(!parse_ushort_token(argv[start + k], &chParsed)) {
						fprintf(stderr, "Invalid channel index: '%s'\n", argv[start + k]);
						free(chList);
						return 2;
					}
					chList[k] = chParsed;
				}
				chCount = count;
				i = j - 1;
			}
		} else if(is_flag(argv[i])) {
			/* Treat as a parameter assignment: --ParamName VALUE */
			const char *flag = argv[i];
			const char *name = flag + 2;
			if(name[0] == '\0') {
				fprintf(stderr, "Invalid flag '%s'\n", flag);
				return 2;
			}
			/* If this is a read/write parameter and no value provided, treat as getter */
			if(is_readwrite_param(name) && (i + 1 >= argc || is_flag(argv[i+1]))) {
				if(add_get_param(name, getParams, &getCount, (int)(sizeof(getParams)/sizeof(getParams[0]))) != 0)
					return 2;
				continue;
			}
			if(i + 1 >= argc || is_flag(argv[i+1])) {
				fprintf(stderr, "Missing value for parameter '%s'\n", name);
				return 2;
			}
			if(add_set_param(params, &paramCount, (int)(sizeof(params)/sizeof(params[0])), name, argv[i+1]) != 0)
				return 2;
			i++;
		} else {
			fprintf(stderr, "Unrecognized argument '%s'\n", argv[i]);
			return 2;
		}
	}

	/* Minimal validation */
	/* Apply connection defaults from config header if present (CLI overrides) */
	{
		CAENHV_SYSTEM_TYPE_t confSys = sysType;
		int confLink = linkType;
		int confSlot = slot;
		const char *cfgPathForConn = (configPath ? configPath : DEFAULT_CONFIG_PATH);
		int cm = read_conn_from_ch_config(cfgPathForConn, &confSys, &confLink, cfgHost, sizeof(cfgHost), cfgUser, sizeof(cfgUser), cfgPass, sizeof(cfgPass), &confSlot);
		if(cm) {
			if((cm & 1) && sysType == DEFAULT_SYSTEM) sysType = confSys;
			if((cm & 2) && linkType == DEFAULT_LINK) linkType = confLink;
			if((cm & 32) && slot < 0) slot = confSlot;
		}
	}

	if(!chAll && (chCount <= 0 || chList == NULL)) {
		/* If a Pw setter is present, fallback to config file to build channel list */
		int hasPwSetter = 0;
		for(i = 0; i < paramCount; i++) {
			if(str_ieq(params[i].name, "Pw")) { hasPwSetter = 1; break; }
		}
		if(hasPwSetter) {
			unsigned short *cfgCh = NULL;
			float *cfgV0 = NULL;
			float *cfgI0 = NULL;
			int cfgCount = 0;
			int lr = -1;
			if(configPath) lr = load_config_file(configPath, &cfgCh, &cfgCount, &cfgV0, &cfgI0, NULL, NULL, NULL, NULL);
			if(lr < 0) lr = load_default_config(&cfgCh, &cfgCount, &cfgV0, &cfgI0, NULL, NULL, NULL, NULL);
			if(lr <= 0) {
				fprintf(stderr, "No channels provided and config not found or empty. Provide --ch or a valid config.\n");
				return 2;
			}
			/* adopt channels from config; free value arrays here and reload later when setting */
			chList = cfgCh;
			chCount = cfgCount;
			free(cfgV0);
			free(cfgI0);
			/* no svmax/names to free here since we passed NULL */
		} else {
			fprintf(stderr, "Missing channels: use --ch <list>\n");
			print_cli_usage(argv[0]);
			return 2;
		}
	}
	if(slot < 0) {
		slot = DEFAULT_SLOT; /* default slot in code */
	}
	if(getCount <= 0 && paramCount <= 0) {
		fprintf(stderr, "Nothing to do. Provide getters like --IMon or setters like --V0Set 650\n");
		print_cli_usage(argv[0]);
		return 2;
	}
	if(getCount > 0 && paramCount > 0) {
		fprintf(stderr, "Cannot mix setters and getters in the same call. Use either --get <Param> or set flags.\n");
		return 2;
	}

	/* Prepare connection Arg */
	char connArg[256];
	memset(connArg, 0, sizeof(connArg));
	/* TCP/IP host (overridable with --host) */
	{
		const char *hostSrc = NULL;
		if(cliHostProvided) hostSrc = host;
		else if(cfgHost[0] != '\0') hostSrc = cfgHost;
		else hostSrc = DEFAULT_HOST;
		snprintf(connArg, sizeof(connArg), "%s", hostSrc);
	}

	/* Username/password defaults: match interactive logic */
	char userBuf[64] = {0};
	char passBuf[64] = {0};
	{
		const char *userSrc = user ? user : (cfgUser[0] ? cfgUser : DEFAULT_USER);
		const char *passSrc = pass ? pass : (cfgPass[0] ? cfgPass : DEFAULT_PASS);
		snprintf(userBuf, sizeof(userBuf), "%s", userSrc);
		snprintf(passBuf, sizeof(passBuf), "%s", passSrc);
	}

	int handle = -1;
	CAENHVRESULT ret = CAENHV_InitSystem(sysType, linkType, (void*)connArg, userBuf, passBuf, &handle);
	if(ret != CAENHV_OK) {
		fprintf(stderr, "CAENHV_InitSystem failed: %s (code %d)\n", CAENHV_GetError(handle), ret);
		free(chList);
		return (int)ret;
	}

	/* Expand channels if '--ch all' */
	if(chAll) {
		unsigned short NrOfCh = 0;
		int haveChannelCount = 0;

		{
			unsigned short serNumb = 0;
			unsigned char fmwMin = 0, fmwMax = 0;
			char Model[32] = {0}, Descr[64] = {0};
			char *mdl = (char*)Model;
			char *des = (char*)Descr;
			CAENHVRESULT tr = CAENHV_TestBdPresence(handle, (unsigned short)slot,
			                                        &NrOfCh, &mdl, &des,
			                                        &serNumb, &fmwMin, &fmwMax);
			if(tr == CAENHV_OK) {
				haveChannelCount = (NrOfCh > 0);
			} else if(tr != CAENHV_INVALIDPARAMETER && tr != CAENHV_FUNCTIONNOTAVAILABLE) {
				/* 다른 에러는 그대로 리턴 */
				fprintf(stderr, "CAENHV_TestBdPresence failed: %s (code %d)\n",
				        CAENHV_GetError(handle), tr);
				CAENHV_DeinitSystem(handle);
				return (int)tr;
			}
		}


		if(!haveChannelCount) {
			unsigned short nrSlots = 0;
			unsigned short *nrChList = NULL;
			unsigned short *serList = NULL;
			unsigned char *fmwMinList = NULL, *fmwMaxList = NULL;
			char *modelList = NULL, *descList = NULL;

			CAENHVRESULT mr = CAENHV_GetCrateMap(handle,
			                                     &nrSlots,
			                                     &nrChList,
			                                     &modelList,
			                                     &descList,
			                                     &serList,
			                                     &fmwMinList,
			                                     &fmwMaxList);
			if(mr == CAENHV_OK &&
			   slot >= 0 && slot < (int)nrSlots &&
			   nrChList != NULL &&
			   nrChList[slot] > 0) {
				NrOfCh = nrChList[slot];
				haveChannelCount = 1;
			}

			if(nrChList)     CAENHV_Free(nrChList);
			if(modelList)    CAENHV_Free(modelList);
			if(descList)     CAENHV_Free(descList);
			if(serList)      CAENHV_Free(serList);
			if(fmwMinList)   CAENHV_Free(fmwMinList);
			if(fmwMaxList)   CAENHV_Free(fmwMaxList);
		}

		if(!haveChannelCount) {
			unsigned short *cfgCh = NULL;
			float *cfgV0 = NULL;
			float *cfgI0 = NULL;
			int cfgCount = 0;
			int lr = -1;

			if(configPath)
				lr = load_config_file(configPath, &cfgCh, &cfgCount, &cfgV0, &cfgI0, NULL, NULL, NULL, NULL);
			if(lr < 0)
				lr = load_default_config(&cfgCh, &cfgCount, &cfgV0, &cfgI0, NULL, NULL, NULL, NULL);

			if(lr <= 0 || cfgCh == NULL || cfgCount <= 0) {
				fprintf(stderr, "Unable to determine channel list for '--ch all'. "
				                "Provide explicit --ch list or a valid config file.\n");
				free(cfgCh);
				free(cfgV0);
				free(cfgI0);
				CAENHV_DeinitSystem(handle);
				return 2;
			}

			chList = cfgCh;
			chCount = cfgCount;
			free(cfgV0);
			free(cfgI0);
		} else {
			unsigned short includeCount = 0;
			for(unsigned short c = 0; c < NrOfCh; c++)
				if(!is_channel_excluded(c))
					includeCount++;
			if(includeCount == 0) {
				fprintf(stderr, "No channels to operate on: all channels are excluded by configuration.\n");
				CAENHV_DeinitSystem(handle);
				return 2;
			}
			chList = (unsigned short*)malloc(sizeof(unsigned short) * (size_t)includeCount);
			if(!chList) {
				fprintf(stderr, "Out of memory\n");
				CAENHV_DeinitSystem(handle);
				return 3;
			}
			unsigned short w = 0;
			for(unsigned short c = 0; c < NrOfCh; c++)
				if(!is_channel_excluded(c))
					chList[w++] = c;
			chCount = includeCount;
		}
	}

	int exitCode = 0;
	if(getCount > 0) {
		typedef struct {
			const char *name;
			int isNumeric;
			int ok;
			float *fVals;
			uint32_t *uVals;
		} fetched_param_t;
		fetched_param_t fetched[32];
		int nf = 0;
		/* Load config names (optional) to print alongside channel index */
		unsigned short *cfgChForNames = NULL;
		int cfgCntForNames = 0;
		float *cfgTmpV0 = NULL, *cfgTmpI0 = NULL, *cfgTmpSV = NULL;
		char **cfgNames = NULL;
		int lrNames = -1;
		if(configPath)
			lrNames = load_config_file(configPath, &cfgChForNames, &cfgCntForNames, &cfgTmpV0, &cfgTmpI0, &cfgTmpSV, NULL, NULL, &cfgNames);
		if(lrNames < 0)
			lrNames = load_default_config(&cfgChForNames, &cfgCntForNames, &cfgTmpV0, &cfgTmpI0, &cfgTmpSV, NULL, NULL, &cfgNames);
		/* Fetch all requested parameters first */
		for(int gi = 0; gi < getCount; gi++) {
			const char *par = getParams[gi];
			unsigned long type = 0;
			fetched[nf].name = par;
			fetched[nf].isNumeric = 0;
			fetched[nf].ok = 0;
			fetched[nf].fVals = NULL;
			fetched[nf].uVals = NULL;
			CAENHVRESULT pr = CAENHV_GetChParamProp(handle, (unsigned short)slot, chList[0], par, "Type", &type);
			if(pr != CAENHV_OK) {
				fprintf(stderr, "GetChParamProp('%s','Type') failed: %s (code %d)\n", par, CAENHV_GetError(handle), pr);
				if(exitCode == 0) exitCode = (int)pr;
				nf++;
				continue;
			}
			if(type == PARAM_TYPE_NUMERIC) {
				fetched[nf].isNumeric = 1;
				fetched[nf].fVals = (float*)malloc(sizeof(float) * (size_t)chCount);
				CHECK_ALLOC_SETEXIT(fetched[nf].fVals, exitCode);
				if(!fetched[nf].fVals) { nf++; continue; }
				CAENHVRESULT gr = CAENHV_GetChParam(handle, (unsigned short)slot, par, (unsigned short)chCount, chList, fetched[nf].fVals);
				if(gr != CAENHV_OK) {
					fprintf(stderr, "GetChParam('%s') failed: %s (code %d)\n", par, CAENHV_GetError(handle), gr);
					if(exitCode == 0) exitCode = (int)gr;
					free(fetched[nf].fVals);
					fetched[nf].fVals = NULL;
				} else {
					fetched[nf].ok = 1;
				}
			} else {
				fetched[nf].uVals = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)chCount);
				CHECK_ALLOC_SETEXIT(fetched[nf].uVals, exitCode);
				if(!fetched[nf].uVals) { nf++; continue; }
				CAENHVRESULT gr = CAENHV_GetChParam(handle, (unsigned short)slot, par, (unsigned short)chCount, chList, fetched[nf].uVals);
				if(gr != CAENHV_OK) {
					fprintf(stderr, "GetChParam('%s') failed: %s (code %d)\n", par, CAENHV_GetError(handle), gr);
					if(exitCode == 0) exitCode = (int)gr;
					free(fetched[nf].uVals);
					fetched[nf].uVals = NULL;
				} else {
					fetched[nf].ok = 1;
				}
			}
			nf++;
		}
		/* Print one line per channel with all fetched params */
		for(int k = 0; k < chCount; k++) {
			/* Try to find a display name for this channel from config */
			const char *dispName = NULL;
			if(lrNames > 0 && cfgChForNames && cfgNames) {
				for(int ci = 0; ci < cfgCntForNames; ci++) {
					if(cfgChForNames[ci] == chList[k]) {
						dispName = cfgNames[ci];
						break;
					}
				}
			}
			if(dispName && *dispName)
				printf("%02d-%02d (%s)", slot, chList[k], dispName);
			else
				printf("%02d-%02d", slot, chList[k]);
			for(int fi = 0; fi < nf; fi++) {
				if(!fetched[fi].ok) continue;
				if(fetched[fi].isNumeric) {
					/* Show VMon/IMon/V0Set/I0Set with 2 decimal places; others keep 6 */
					if(str_ieq(fetched[fi].name, "VMon") || str_ieq(fetched[fi].name, "IMon") ||
						str_ieq(fetched[fi].name, "V0Set") || str_ieq(fetched[fi].name, "I0Set")) {
						printf("  %s = %.2f", fetched[fi].name, (double)fetched[fi].fVals[k]);
					} else {
						printf("  %s = %.6f", fetched[fi].name, (double)fetched[fi].fVals[k]);
					}
				} else {
					uint32_t v = fetched[fi].uVals[k];
					if(str_ieq(fetched[fi].name, "Status")) {
						// printf("  %s = %x (%s)", fetched[fi].name, (unsigned int)v, status_label(v));
						printf("  %s = %s", fetched[fi].name, status_label(v));
					} else {
						printf("  %s = %x", fetched[fi].name, (unsigned int)v);
					}
				}
			}
			printf("\n");
		}
		/* Cleanup */
		for(int fi = 0; fi < nf; fi++) {
			if(fetched[fi].fVals) free(fetched[fi].fVals);
			if(fetched[fi].uVals) free(fetched[fi].uVals);
		}
		/* Free config name resources */
		if(cfgNames) {
			for(int ci = 0; ci < cfgCntForNames; ci++) free(cfgNames[ci]);
			free(cfgNames);
		}
		free(cfgChForNames);
		free(cfgTmpV0);
		free(cfgTmpI0);
		free(cfgTmpSV);
	} else {
		/* Set mode */
		/* If turning power On/Off and channels came from config, apply V0Set/I0Set per-channel from config first */
		{
			int hasPwSetter2 = 0;
			for(int pi = 0; pi < paramCount; pi++) if(str_ieq(params[pi].name, "Pw")) { hasPwSetter2 = 1; break; }
			if(hasPwSetter2) {
				unsigned short *cfgCh = NULL;
				float *cfgV0 = NULL;
				float *cfgI0 = NULL;
				float *cfgSV = NULL;
				float *cfgRU = NULL;
				float *cfgRD = NULL;
				int cfgCount = 0;
				int lr = -1;
				if(configPath) lr = load_config_file(configPath, &cfgCh, &cfgCount, &cfgV0, &cfgI0, &cfgSV, &cfgRU, &cfgRD, NULL);
				if(lr < 0) lr = load_default_config(&cfgCh, &cfgCount, &cfgV0, &cfgI0, &cfgSV, &cfgRU, &cfgRD, NULL);
				if(lr > 0) {
					for(int idx = 0; idx < cfgCount; idx++) {
						unsigned short oneCh = cfgCh[idx];
						/* Apply SVMax if provided (> 0) before V/I */
						if(cfgSV && cfgSV[idx] > 0.0f) {
							float sv = cfgSV[idx];
							CAENHVRESULT sr0 = CAENHV_SetChParam(handle, (unsigned short)slot, "SVMax", 1, &oneCh, &sv);
							if(sr0 != CAENHV_OK) {
								fprintf(stderr, "SetChParam('SVMax', %.3f) ch %u failed: %s (code %d)\n", (double)sv, oneCh, CAENHV_GetError(handle), sr0);
								exitCode = (int)sr0;
							}
						}
						if(cfgRU && cfgRU[idx] > 0.0f) {
							float ru = cfgRU[idx];
							CAENHVRESULT sr = CAENHV_SetChParam(handle, (unsigned short)slot, "RUp", 1, &oneCh, &ru);
							if(sr != CAENHV_OK) {
								fprintf(stderr, "SetChParam('RUp', %.3f) ch %u failed: %s (code %d)\n", (double)ru, oneCh, CAENHV_GetError(handle), sr);
								exitCode = (int)sr;
							}
						}
						if(cfgRD && cfgRD[idx] > 0.0f) {
							float rd = cfgRD[idx];
							CAENHVRESULT sr = CAENHV_SetChParam(handle, (unsigned short)slot, "RDWn", 1, &oneCh, &rd);
							if(sr != CAENHV_OK) {
								fprintf(stderr, "SetChParam('RDWn', %.3f) ch %u failed: %s (code %d)\n", (double)rd, oneCh, CAENHV_GetError(handle), sr);
								exitCode = (int)sr;
							}
						}
						float v0 = cfgV0[idx];
						float i0 = cfgI0[idx];
						CAENHVRESULT sr1 = CAENHV_SetChParam(handle, (unsigned short)slot, "V0Set", 1, &oneCh, &v0);
						if(sr1 != CAENHV_OK) {
							fprintf(stderr, "SetChParam('V0Set', %.3f) ch %u failed: %s (code %d)\n", (double)v0, oneCh, CAENHV_GetError(handle), sr1);
							exitCode = (int)sr1;
						}
						CAENHVRESULT sr2 = CAENHV_SetChParam(handle, (unsigned short)slot, "I0Set", 1, &oneCh, &i0);
						if(sr2 != CAENHV_OK) {
							fprintf(stderr, "SetChParam('I0Set', %.3f) ch %u failed: %s (code %d)\n", (double)i0, oneCh, CAENHV_GetError(handle), sr2);
							exitCode = (int)sr2;
						}
					}
				}
				free(cfgCh);
				free(cfgV0);
				free(cfgI0);
				free(cfgSV);
				free(cfgRU);
				free(cfgRD);
			}
		}
		for(i = 0; i < paramCount; i++) {
			unsigned long type = 0;
			CAENHVRESULT pr = CAENHV_GetChParamProp(handle, (unsigned short)slot, chList[0], params[i].name, "Type", &type);
			if(pr != CAENHV_OK) {
				fprintf(stderr, "GetChParamProp('%s','Type') failed: %s (code %d)\n", params[i].name, CAENHV_GetError(handle), pr);
				exitCode = (int)pr;
				break;
			}

			if(type == PARAM_TYPE_NUMERIC) {
				float fVal = (float)atof(params[i].value);
				CAENHVRESULT sr = CAENHV_SetChParam(handle, (unsigned short)slot, params[i].name, (unsigned short)chCount, chList, &fVal);
				if(sr != CAENHV_OK) {
					fprintf(stderr, "SetChParam('%s', %f) failed: %s (code %d)\n", params[i].name, fVal, CAENHV_GetError(handle), sr);
					exitCode = (int)sr;
					break;
				}
				printf("OK: %s = %g applied to %d channel(s)\n", params[i].name, (double)fVal, chCount);
			} else if(type == PARAM_TYPE_ONOFF) {
				unsigned long lVal = 0;
				if(str_ieq(params[i].value, "on")) lVal = 1;
				else if(str_ieq(params[i].value, "off")) lVal = 0;
				else lVal = (unsigned long)strtoul(params[i].value, NULL, 0);
				CAENHVRESULT sr = CAENHV_SetChParam(handle, (unsigned short)slot, params[i].name, (unsigned short)chCount, chList, &lVal);
				if(sr != CAENHV_OK) {
					fprintf(stderr, "SetChParam('%s', %lu) failed: %s (code %d)\n", params[i].name, lVal, CAENHV_GetError(handle), sr);
					exitCode = (int)sr;
					break;
				}
				printf("OK: %s = %lu applied to %d channel(s)\n", params[i].name, lVal, chCount);
			} else {
				/* Default: treat as integer/enum */
				unsigned long lVal = (unsigned long)strtoul(params[i].value, NULL, 0);
				CAENHVRESULT sr = CAENHV_SetChParam(handle, (unsigned short)slot, params[i].name, (unsigned short)chCount, chList, &lVal);
				if(sr != CAENHV_OK) {
					fprintf(stderr, "SetChParam('%s', %lu) failed: %s (code %d)\n", params[i].name, lVal, CAENHV_GetError(handle), sr);
					exitCode = (int)sr;
					break;
				}
				printf("OK: %s = %lu applied to %d channel(s)\n", params[i].name, lVal, chCount);
			}
		}
	}

	CAENHVRESULT dr = CAENHV_DeinitSystem(handle);
	if(dr != CAENHV_OK) {
		fprintf(stderr, "CAENHV_DeinitSystem: %s (code %d)\n", CAENHV_GetError(handle), dr);
		if(exitCode == 0) exitCode = (int)dr;
	}

	free(chList);
	return exitCode;
}

int main(int argc, char **argv)
{
	int   ret;
	char  esc = 0;

    loop = 0;

	/* CLI mode: if args are provided, run non-interactive flow */
	if(argc > 1) {
		return run_cli(argc, argv);
	}

	for( ret = 0; ret < MAX_HVPS ; ret++ )
		System[ret].ID = -1;

	con_init();
	commandList();
	con_end();

	return 0;
}
