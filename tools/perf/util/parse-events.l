
%option reentrant
%option bison-bridge
%option prefix="parse_events_"
%option stack
%option bison-locations
%option yylineno
%option reject

%{
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "parse-events.h"
#include "parse-events-bison.h"
#include "evsel.h"

char *parse_events_get_text(yyscan_t yyscanner);
YYSTYPE *parse_events_get_lval(yyscan_t yyscanner);
int parse_events_get_column(yyscan_t yyscanner);
int parse_events_get_leng(yyscan_t yyscanner);

static int get_column(yyscan_t scanner)
{
	return parse_events_get_column(scanner) - parse_events_get_leng(scanner);
}

static int value(struct parse_events_state *parse_state, yyscan_t scanner, int base)
{
	YYSTYPE *yylval = parse_events_get_lval(scanner);
	char *text = parse_events_get_text(scanner);
	u64 num;

	errno = 0;
	num = strtoull(text, NULL, base);
	if (errno) {
		struct parse_events_error *error = parse_state->error;
		char *help = NULL;

		if (asprintf(&help, "Bad base %d number \"%s\"", base, text) > 0)
			parse_events_error__handle(error, get_column(scanner), help , NULL);

		return PE_ERROR;
	}

	yylval->num = num;
	return PE_VALUE;
}

static int str(yyscan_t scanner, int token)
{
	YYSTYPE *yylval = parse_events_get_lval(scanner);
	char *text = parse_events_get_text(scanner);

	yylval->str = strdup(text);
	return token;
}

static int quoted_str(yyscan_t scanner, int token)
{
	YYSTYPE *yylval = parse_events_get_lval(scanner);
	char *text = parse_events_get_text(scanner);

	/*
	 * If a text tag specified on the command line
	 * contains opening single quite ' then it is
	 * expected that the tag ends with single quote
	 * as well, like this:
	 *     name=\'CPU_CLK_UNHALTED.THREAD:cmask=1\'
	 * quotes need to be escaped to bypass shell
	 * processing.
	 */
	yylval->str = strndup(&text[1], strlen(text) - 2);
	return token;
}

static int lc_str(yyscan_t scanner, const struct parse_events_state *state)
{
	return str(scanner, state->match_legacy_cache_terms ? PE_LEGACY_CACHE : PE_NAME);
}

/*
 * This function is called when the parser gets two kind of input:
 *
 * 	@cfg1 or @cfg2=config
 *
 * The leading '@' is stripped off before 'cfg1' and 'cfg2=config' are given to
 * bison.  In the latter case it is necessary to keep the string intact so that
 * the PMU kernel driver can determine what configurable is associated to
 * 'config'.
 */
static int drv_str(yyscan_t scanner, int token)
{
	YYSTYPE *yylval = parse_events_get_lval(scanner);
	char *text = parse_events_get_text(scanner);

	/* Strip off the '@' */
	yylval->str = strdup(text + 1);
	return token;
}

/*
 * Use yyless to return all the characaters to the input. Update the column for
 * location debugging. If __alloc is non-zero set yylval to the text for the
 * returned token's value.
 */
#define REWIND(__alloc)				\
do {								\
	YYSTYPE *__yylval = parse_events_get_lval(yyscanner);	\
	char *text = parse_events_get_text(yyscanner);		\
								\
	if (__alloc)						\
		__yylval->str = strdup(text);			\
								\
	yycolumn -= strlen(text);				\
	yyless(0);						\
} while (0)

static int sym(yyscan_t scanner, int config)
{
	YYSTYPE *yylval = parse_events_get_lval(scanner);

	yylval->num = config;
	return PE_VALUE_SYM_HW;
}

static int term(yyscan_t scanner, enum parse_events__term_type type)
{
	YYSTYPE *yylval = parse_events_get_lval(scanner);

	yylval->term_type = type;
	return PE_TERM;
}

static int hw_term(yyscan_t scanner, int config)
{
	YYSTYPE *yylval = parse_events_get_lval(scanner);
	char *text = parse_events_get_text(scanner);

	yylval->hardware_term.str = strdup(text);
	yylval->hardware_term.num = PERF_TYPE_HARDWARE + config;
	return PE_TERM_HW;
}

static void modifiers_error(struct parse_events_state *parse_state, yyscan_t scanner,
			    int pos, char mod_char, const char *mod_name)
{
	struct parse_events_error *error = parse_state->error;
	char *help = NULL;

	if (asprintf(&help, "Duplicate modifier '%c' (%s)", mod_char, mod_name) > 0)
		parse_events_error__handle(error, get_column(scanner) + pos, help , NULL);
}

static int modifiers(struct parse_events_state *parse_state, yyscan_t scanner)
{
	YYSTYPE *yylval = parse_events_get_lval(scanner);
	char *text = parse_events_get_text(scanner);
	struct parse_events_modifier mod = { .precise = 0, };

	for (size_t i = 0, n = strlen(text); i < n; i++) {
#define CASE(c, field)							\
		case c:							\
			if (mod.field) {				\
				modifiers_error(parse_state, scanner, i, c, #field); \
				return PE_ERROR;			\
			}						\
			mod.field = true;				\
			break

		switch (text[i]) {
		CASE('u', user);
		CASE('k', kernel);
		CASE('h', hypervisor);
		CASE('I', non_idle);
		CASE('G', guest);
		CASE('H', host);
		case 'p':
			mod.precise++;
			/*
			 * precise ip:
			 *
			 *  0 - SAMPLE_IP can have arbitrary skid
			 *  1 - SAMPLE_IP must have constant skid
			 *  2 - SAMPLE_IP requested to have 0 skid
			 *  3 - SAMPLE_IP must have 0 skid
			 *
			 *  See also PERF_RECORD_MISC_EXACT_IP
			 */
			if (mod.precise > 3) {
				struct parse_events_error *error = parse_state->error;
				char *help = strdup("Maximum precise value is 3");

				if (help) {
					parse_events_error__handle(error, get_column(scanner) + i,
								   help , NULL);
				}
				return PE_ERROR;
			}
			break;
		CASE('P', precise_max);
		CASE('S', sample_read);
		CASE('D', pinned);
		CASE('W', weak);
		CASE('e', exclusive);
		CASE('b', bpf);
		CASE('R', retire_lat);
		default:
			return PE_ERROR;
		}
#undef CASE
	}
	yylval->mod = mod;
	return PE_MODIFIER_EVENT;
}

#define YY_USER_ACTION					\
do {							\
	yylloc->last_column  = yylloc->first_column;	\
	yylloc->first_column = yycolumn;		\
	yycolumn += yyleng;				\
} while (0);

#define USER_REJECT		\
	yycolumn -= yyleng;	\
	REJECT

%}

%x mem
%s config
%x event

group		[^,{}/]*[{][^}]*[}][^,{}/]*
event_pmu	[^,{}/]+[/][^/]*[/][^,{}/]*
event		[^,{}/]+

num_dec		[0-9]+
num_hex		0x[a-fA-F0-9]{1,16}
num_raw_hex	[a-fA-F0-9]{1,16}
/* Regular pattern to match the token PE_NAME. */
name_start      [a-zA-Z0-9_*?\[\]]
name		{name_start}[a-zA-Z0-9_*?.\[\]!\-]*
/* PE_NAME token when inside a config term list, allows ':'. */
term_name	{name_start}[a-zA-Z0-9_*?.\[\]!\-:]*
/*
 * PE_NAME token when quoted, allows ':,.='.
 * Matches the RHS of terms like: name='COMPLEX_CYCLES_NAME:orig=cycles,desc=chip-clock-ticks'.
 */
quoted_name	[\']{name_start}[a-zA-Z0-9_*?.\[\]!\-:,\.=]*[\']
drv_cfg_term	[a-zA-Z0-9_\.]+(=[a-zA-Z0-9_*?\.:]+)?
/*
 * If you add a modifier you need to update check_modifier().
 * Also, the letters in modifier_event must not be in modifier_bp.
 */
modifier_event	[ukhpPGHSDIWebR]{1,16}
modifier_bp	[rwx]{1,3}
lc_type 	(L1-dcache|l1-d|l1d|L1-data|L1-icache|l1-i|l1i|L1-instruction|LLC|L2|dTLB|d-tlb|Data-TLB|iTLB|i-tlb|Instruction-TLB|branch|branches|bpu|btb|bpc|node)
lc_op_result	(load|loads|read|store|stores|write|prefetch|prefetches|speculative-read|speculative-load|refs|Reference|ops|access|misses|miss)
digit		[0-9]
non_digit	[^0-9]

%%

%{
	struct parse_events_state *_parse_state = parse_events_get_extra(yyscanner);
	{
		int start_token = _parse_state->stoken;

		if (start_token == PE_START_TERMS)
			BEGIN(config);
		else if (start_token == PE_START_EVENTS)
			BEGIN(event);

		if (start_token) {
			_parse_state->stoken = 0;
			/*
			 * The flex parser does not init locations variable
			 * via the scan_string interface, so we need do the
			 * init in here.
			 */
			yycolumn = 0;
			return start_token;
		}
         }
%}

<event>{

{group}		{
			BEGIN(INITIAL);
			REWIND(0);
		}

{event_pmu}	|
{event}		{
			BEGIN(INITIAL);
			REWIND(1);
			return PE_EVENT_NAME;
		}

<<EOF>>		{
			BEGIN(INITIAL);
			REWIND(0);
		}
,		{
			return ',';
		}
}

<config>{
	/*
	 * Please update config_term_names when new static term is added.
	 */
config			{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_CONFIG); }
config1			{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_CONFIG1); }
config2			{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_CONFIG2); }
config3			{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_CONFIG3); }
name			{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_NAME); }
period			{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_SAMPLE_PERIOD); }
freq			{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_SAMPLE_FREQ); }
branch_type		{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_BRANCH_SAMPLE_TYPE); }
time			{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_TIME); }
call-graph		{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_CALLGRAPH); }
stack-size		{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_STACKSIZE); }
max-stack		{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_MAX_STACK); }
nr			{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_MAX_EVENTS); }
inherit			{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_INHERIT); }
no-inherit		{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_NOINHERIT); }
overwrite		{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_OVERWRITE); }
no-overwrite		{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_NOOVERWRITE); }
percore			{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_PERCORE); }
aux-output		{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_AUX_OUTPUT); }
aux-action		{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_AUX_ACTION); }
aux-sample-size		{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_AUX_SAMPLE_SIZE); }
metric-id		{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_METRIC_ID); }
cpu			{ return term(yyscanner, PARSE_EVENTS__TERM_TYPE_CPU); }
cpu-cycles|cycles				{ return hw_term(yyscanner, PERF_COUNT_HW_CPU_CYCLES); }
stalled-cycles-frontend|idle-cycles-frontend	{ return hw_term(yyscanner, PERF_COUNT_HW_STALLED_CYCLES_FRONTEND); }
stalled-cycles-backend|idle-cycles-backend	{ return hw_term(yyscanner, PERF_COUNT_HW_STALLED_CYCLES_BACKEND); }
instructions					{ return hw_term(yyscanner, PERF_COUNT_HW_INSTRUCTIONS); }
cache-references				{ return hw_term(yyscanner, PERF_COUNT_HW_CACHE_REFERENCES); }
cache-misses					{ return hw_term(yyscanner, PERF_COUNT_HW_CACHE_MISSES); }
branch-instructions|branches			{ return hw_term(yyscanner, PERF_COUNT_HW_BRANCH_INSTRUCTIONS); }
branch-misses					{ return hw_term(yyscanner, PERF_COUNT_HW_BRANCH_MISSES); }
bus-cycles					{ return hw_term(yyscanner, PERF_COUNT_HW_BUS_CYCLES); }
ref-cycles					{ return hw_term(yyscanner, PERF_COUNT_HW_REF_CPU_CYCLES); }
r{num_raw_hex}		{ return str(yyscanner, PE_RAW); }
r0x{num_raw_hex}	{ return str(yyscanner, PE_RAW); }
,			{ return ','; }
"/"			{ BEGIN(INITIAL); return '/'; }
{lc_type}			{ return lc_str(yyscanner, _parse_state); }
{lc_type}-{lc_op_result}	{ return lc_str(yyscanner, _parse_state); }
{lc_type}-{lc_op_result}-{lc_op_result}	{ return lc_str(yyscanner, _parse_state); }
{num_dec}		{ return value(_parse_state, yyscanner, 10); }
{num_hex}		{ return value(_parse_state, yyscanner, 16); }
{term_name}		{ return str(yyscanner, PE_NAME); }
@{drv_cfg_term}		{ return drv_str(yyscanner, PE_DRV_CFG_TERM); }
}

<mem>{
{modifier_bp}		{ return str(yyscanner, PE_MODIFIER_BP); }
	/*
	 * The colon before memory access modifiers can get mixed up with the
	 * colon before event modifiers. Fortunately none of the option letters
	 * are the same, so trailing context can be used disambiguate the two
	 * cases.
	 */
":"/{modifier_bp}	{ return PE_BP_COLON; }
	/*
	 * The slash before memory length can get mixed up with the slash before
	 * config terms. Fortunately config terms do not start with a numeric
	 * digit, so trailing context can be used disambiguate the two cases.
	 */
"/"/{digit}		{ return PE_BP_SLASH; }
"/"/{non_digit}		{ BEGIN(config); return '/'; }
{num_dec}		{ return value(_parse_state, yyscanner, 10); }
{num_hex}		{ return value(_parse_state, yyscanner, 16); }
	/*
	 * We need to separate 'mem:' scanner part, in order to get specific
	 * modifier bits parsed out. Otherwise we would need to handle PE_NAME
	 * and we'd need to parse it manually. During the escape from <mem>
	 * state we need to put the escaping char back, so we dont miss it.
	 */
.			{ unput(*yytext); BEGIN(INITIAL); }
	/*
	 * We destroy the scanner after reaching EOF,
	 * but anyway just to be sure get back to INIT state.
	 */
<<EOF>>			{ BEGIN(INITIAL); }
}

cpu-cycles|cycles				{ return sym(yyscanner, PERF_COUNT_HW_CPU_CYCLES); }
stalled-cycles-frontend|idle-cycles-frontend	{ return sym(yyscanner, PERF_COUNT_HW_STALLED_CYCLES_FRONTEND); }
stalled-cycles-backend|idle-cycles-backend	{ return sym(yyscanner, PERF_COUNT_HW_STALLED_CYCLES_BACKEND); }
instructions					{ return sym(yyscanner, PERF_COUNT_HW_INSTRUCTIONS); }
cache-references				{ return sym(yyscanner, PERF_COUNT_HW_CACHE_REFERENCES); }
cache-misses					{ return sym(yyscanner, PERF_COUNT_HW_CACHE_MISSES); }
branch-instructions|branches			{ return sym(yyscanner, PERF_COUNT_HW_BRANCH_INSTRUCTIONS); }
branch-misses					{ return sym(yyscanner, PERF_COUNT_HW_BRANCH_MISSES); }
bus-cycles					{ return sym(yyscanner, PERF_COUNT_HW_BUS_CYCLES); }
ref-cycles					{ return sym(yyscanner, PERF_COUNT_HW_REF_CPU_CYCLES); }

{lc_type}			{ return str(yyscanner, PE_LEGACY_CACHE); }
{lc_type}-{lc_op_result}	{ return str(yyscanner, PE_LEGACY_CACHE); }
{lc_type}-{lc_op_result}-{lc_op_result}	{ return str(yyscanner, PE_LEGACY_CACHE); }
mem:			{ BEGIN(mem); return PE_PREFIX_MEM; }
r{num_raw_hex}		{ return str(yyscanner, PE_RAW); }
{num_dec}		{ return value(_parse_state, yyscanner, 10); }
{num_hex}		{ return value(_parse_state, yyscanner, 16); }

{modifier_event}	{ return modifiers(_parse_state, yyscanner); }
{name}			{ return str(yyscanner, PE_NAME); }
{quoted_name}		{ return quoted_str(yyscanner, PE_NAME); }
"/"			{ BEGIN(config); return '/'; }
,			{ BEGIN(event); return ','; }
:			{ return ':'; }
"{"			{ BEGIN(event); return '{'; }
"}"			{ return '}'; }
=			{ return '='; }
\n			{ }
.			{ }

%%

int parse_events_wrap(void *scanner __maybe_unused)
{
	return 1;
}
