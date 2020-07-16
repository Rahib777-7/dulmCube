#include "LWeb.h"
#include "Launcher.h"
#include "Platform.h"
#include "Stream.h"
#include "Logger.h"
#include "Window.h"
#include "Options.h"
#include "PackedCol.h"
#include "Errors.h"

#ifndef CC_BUILD_WEB
/*########################################################################################################################*
*----------------------------------------------------------JSON-----------------------------------------------------------*
*#########################################################################################################################*/
#define TOKEN_NONE  0
#define TOKEN_NUM   1
#define TOKEN_TRUE  2
#define TOKEN_FALSE 3
#define TOKEN_NULL  4
/* Consumes n characters from the JSON stream */
#define JsonContext_Consume(ctx, n) ctx->cur += n; ctx->left -= n;

static const String strTrue  = String_FromConst("true");
static const String strFalse = String_FromConst("false");
static const String strNull  = String_FromConst("null");

static cc_bool Json_IsWhitespace(char c) {
	return c == '\r' || c == '\n' || c == '\t' || c == ' ';
}

static cc_bool Json_IsNumber(char c) {
	return c == '-' || c == '.' || (c >= '0' && c <= '9');
}

static cc_bool Json_ConsumeConstant(struct JsonContext* ctx, const String* value) {
	int i;
	if (value->length > ctx->left) return false;

	for (i = 0; i < value->length; i++) {
		if (ctx->cur[i] != value->buffer[i]) return false;
	}

	JsonContext_Consume(ctx, value->length);
	return true;
}

static int Json_ConsumeToken(struct JsonContext* ctx) {
	char c;
	for (; ctx->left && Json_IsWhitespace(*ctx->cur); ) { JsonContext_Consume(ctx, 1); }
	if (!ctx->left) return TOKEN_NONE;

	c = *ctx->cur;
	if (c == '{' || c == '}' || c == '[' || c == ']' || c == ',' || c == '"' || c == ':') {
		JsonContext_Consume(ctx, 1); return c;
	}

	/* number token forms part of value, don't consume it */
	if (Json_IsNumber(c)) return TOKEN_NUM;

	if (Json_ConsumeConstant(ctx, &strTrue))  return TOKEN_TRUE;
	if (Json_ConsumeConstant(ctx, &strFalse)) return TOKEN_FALSE;
	if (Json_ConsumeConstant(ctx, &strNull))  return TOKEN_NULL;

	/* invalid token */
	JsonContext_Consume(ctx, 1);
	return TOKEN_NONE;
}

static String Json_ConsumeNumber(struct JsonContext* ctx) {
	int len = 0;
	for (; ctx->left && Json_IsNumber(*ctx->cur); len++) { JsonContext_Consume(ctx, 1); }
	return String_Init(ctx->cur - len, len, len);
}

static void Json_ConsumeString(struct JsonContext* ctx, String* str) {
	int codepoint, h[4];
	char c;
	str->length = 0;

	for (; ctx->left;) {
		c = *ctx->cur; JsonContext_Consume(ctx, 1);
		if (c == '"') return;
		if (c != '\\') { String_Append(str, c); continue; }

		/* form of \X */
		if (!ctx->left) break;
		c = *ctx->cur; JsonContext_Consume(ctx, 1);
		if (c == '/' || c == '\\' || c == '"') { String_Append(str, c); continue; }

		/* form of \uYYYY */
		if (c != 'u' || ctx->left < 4) break;
		if (!PackedCol_Unhex(ctx->cur, h, 4)) break;

		codepoint = (h[0] << 12) | (h[1] << 8) | (h[2] << 4) | h[3];
		/* don't want control characters in names/software */
		/* TODO: Convert to CP437.. */
		if (codepoint >= 32) String_Append(str, codepoint);
		JsonContext_Consume(ctx, 4);
	}

	ctx->failed = true; str->length = 0;
}
static String Json_ConsumeValue(int token, struct JsonContext* ctx);

static void Json_ConsumeObject(struct JsonContext* ctx) {
	char keyBuffer[STRING_SIZE];
	String value, oldKey = ctx->curKey;
	int token;
	ctx->OnNewObject(ctx);

	while (true) {
		token = Json_ConsumeToken(ctx);
		if (token == ',') continue;
		if (token == '}') return;

		if (token != '"') { ctx->failed = true; return; }
		String_InitArray(ctx->curKey, keyBuffer);
		Json_ConsumeString(ctx, &ctx->curKey);

		token = Json_ConsumeToken(ctx);
		if (token != ':') { ctx->failed = true; return; }

		token = Json_ConsumeToken(ctx);
		if (token == TOKEN_NONE) { ctx->failed = true; return; }

		value = Json_ConsumeValue(token, ctx);
		ctx->OnValue(ctx, &value);
		ctx->curKey = oldKey;
	}
}

static void Json_ConsumeArray(struct JsonContext* ctx) {
	String value;
	int token;
	ctx->OnNewArray(ctx);

	while (true) {
		token = Json_ConsumeToken(ctx);
		if (token == ',') continue;
		if (token == ']') return;

		if (token == TOKEN_NONE) { ctx->failed = true; return; }
		value = Json_ConsumeValue(token, ctx);
		ctx->OnValue(ctx, &value);
	}
}

static String Json_ConsumeValue(int token, struct JsonContext* ctx) {
	switch (token) {
	case '{': Json_ConsumeObject(ctx); break;
	case '[': Json_ConsumeArray(ctx);  break;
	case '"': Json_ConsumeString(ctx, &ctx->_tmp); return ctx->_tmp;

	case TOKEN_NUM:   return Json_ConsumeNumber(ctx);
	case TOKEN_TRUE:  return strTrue;
	case TOKEN_FALSE: return strFalse;
	case TOKEN_NULL:  break;
	}
	return String_Empty;
}

static void Json_NullOnNew(struct JsonContext* ctx) { }
static void Json_NullOnValue(struct JsonContext* ctx, const String* v) { }
void Json_Init(struct JsonContext* ctx, STRING_REF char* str, int len) {
	ctx->cur    = str;
	ctx->left   = len;
	ctx->failed = false;
	ctx->curKey = String_Empty;

	ctx->OnNewArray  = Json_NullOnNew;
	ctx->OnNewObject = Json_NullOnNew;
	ctx->OnValue     = Json_NullOnValue;
	String_InitArray(ctx->_tmp, ctx->_tmpBuffer);
}

void Json_Parse(struct JsonContext* ctx) {
	int token;
	do {
		token = Json_ConsumeToken(ctx);
		Json_ConsumeValue(token, ctx);
	} while (token != TOKEN_NONE);
}

static void Json_Handle(cc_uint8* data, cc_uint32 len, 
						JsonOnValue onVal, JsonOnNew newArr, JsonOnNew newObj) {
	struct JsonContext ctx;
	/* NOTE: classicube.net uses \u JSON for non ASCII, no need to UTF8 convert characters here */
	Json_Init(&ctx, (char*)data, len);
	
	if (onVal)  ctx.OnValue     = onVal;
	if (newArr) ctx.OnNewArray  = newArr;
	if (newObj) ctx.OnNewObject = newObj;
	Json_Parse(&ctx);
}


/*########################################################################################################################*
*--------------------------------------------------------Web task---------------------------------------------------------*
*#########################################################################################################################*/
static void LWebTask_Reset(struct LWebTask* task) {
	task->completed = false;
	task->working   = true;
	task->success   = false;
	task->start     = Stopwatch_Measure();
	task->res       = 0;
	task->status    = 0;
}

void LWebTask_Tick(struct LWebTask* task) {
	struct HttpRequest req;
	cc_uint64 finish;
	int delta;
	if (task->completed) return;

	if (!Http_GetResult(&task->identifier, &req)) return;
	finish = Stopwatch_Measure();
	delta  = (int)Stopwatch_ElapsedMilliseconds(task->start, finish);
	Platform_Log2("%s took %i", &task->identifier, &delta);

	task->res    = req.result;
	task->status = req.statusCode;

	task->working   = false;
	task->completed = true;
	task->success   = req.success;
	if (task->success) task->Handle((cc_uint8*)req.data, req.size);
	HttpRequest_Free(&req);
}

void LWebTask_DisplayError(struct LWebTask* task, const char* action, String* dst) {
	Launcher_DisplayHttpError(task->res, task->status, action, dst);
}


/*########################################################################################################################*
*-------------------------------------------------------GetTokenTask------------------------------------------------------*
*#########################################################################################################################*/
static struct StringsBuffer ccCookies;
struct GetTokenTaskData GetTokenTask;

static void GetTokenTask_OnValue(struct JsonContext* ctx, const String* str) {
	if (!String_CaselessEqualsConst(&ctx->curKey, "token")) return;
	String_Copy(&GetTokenTask.token, str);
}

static void GetTokenTask_Handle(cc_uint8* data, cc_uint32 len) {
	Json_Handle(data, len, GetTokenTask_OnValue, NULL, NULL);
}

void GetTokenTask_Run(void) {
	static const String id  = String_FromConst("CC get token");
	static const String url = String_FromConst("https://www.classicube.net/api/login");
	static char tokenBuffer[STRING_SIZE];
	if (GetTokenTask.Base.working) return;

	LWebTask_Reset(&GetTokenTask.Base);
	String_InitArray(GetTokenTask.token, tokenBuffer);

	GetTokenTask.Base.identifier = id;
	Http_AsyncGetDataEx(&url, false, &id, NULL, NULL, &ccCookies);
	GetTokenTask.Base.Handle     = GetTokenTask_Handle;
}


/*########################################################################################################################*
*--------------------------------------------------------SignInTask-------------------------------------------------------*
*#########################################################################################################################*/
struct SignInTaskData SignInTask;
static char userBuffer[STRING_SIZE];

static void SignInTask_LogError(const String* str) {
	if (String_CaselessEqualsConst(str, "username") || String_CaselessEqualsConst(str, "password")) {
		SignInTask.error = "&cWrong username or password";
	} else if (String_CaselessEqualsConst(str, "verification")) {
		SignInTask.error = "&cAccount verification required";
	} else if (str->length) {
		SignInTask.error = "&cUnknown error occurred";
	}
}

static void SignInTask_OnValue(struct JsonContext* ctx, const String* str) {
	if (String_CaselessEqualsConst(&ctx->curKey, "username")) {
		String_Copy(&SignInTask.username, str);
	} else if (String_CaselessEqualsConst(&ctx->curKey, "errors")) {
		SignInTask_LogError(str);
	}
}

static void SignInTask_Handle(cc_uint8* data, cc_uint32 len) {
	Json_Handle(data, len, SignInTask_OnValue, NULL, NULL);
}

static void SignInTask_Append(String* dst, const char* key, const String* value) {
	String_AppendConst(dst, key);
	Http_UrlEncodeUtf8(dst, value);
}

void SignInTask_Run(const String* user, const String* pass) {
	static const String id  = String_FromConst("CC post login");
	static const String url = String_FromConst("https://www.classicube.net/api/login");
	String args; char argsBuffer[384];
	if (SignInTask.Base.working) return;

	LWebTask_Reset(&SignInTask.Base);
	String_InitArray(SignInTask.username, userBuffer);
	SignInTask.error = NULL;

	String_InitArray(args, argsBuffer);
	SignInTask_Append(&args, "username=",  user);
	SignInTask_Append(&args, "&password=", pass);
	SignInTask_Append(&args, "&token=",    &GetTokenTask.token);

	SignInTask.Base.identifier = id;
	Http_AsyncPostData(&url, false, &id, args.buffer, args.length, &ccCookies);
	SignInTask.Base.Handle     = SignInTask_Handle;
}


/*########################################################################################################################*
*-----------------------------------------------------FetchServerTask-----------------------------------------------------*
*#########################################################################################################################*/
struct FetchServerData FetchServerTask;
static struct ServerInfo* curServer;

static void ServerInfo_Init(struct ServerInfo* info) {
	String_InitArray(info->hash, info->_hashBuffer);
	String_InitArray(info->name, info->_nameBuffer);
	String_InitArray(info->ip,   info->_ipBuffer);
	String_InitArray(info->mppass,   info->_mppassBuffer);
	String_InitArray(info->software, info->_softBuffer);

	info->players    = 0;
	info->maxPlayers = 0;
	info->uptime     = 0;
	info->featured   = false;
	info->country[0] = 't';
	info->country[1] = '1'; /* 'T1' for unrecognised country */
	info->_order     = -100000;
}

static void ServerInfo_Parse(struct JsonContext* ctx, const String* val) {
	struct ServerInfo* info = curServer;
	if (String_CaselessEqualsConst(&ctx->curKey, "hash")) {
		String_Copy(&info->hash, val);
	} else if (String_CaselessEqualsConst(&ctx->curKey, "name")) {
		String_Copy(&info->name, val);
	} else if (String_CaselessEqualsConst(&ctx->curKey, "players")) {
		Convert_ParseInt(val, &info->players);
	} else if (String_CaselessEqualsConst(&ctx->curKey, "maxplayers")) {
		Convert_ParseInt(val, &info->maxPlayers);
	} else if (String_CaselessEqualsConst(&ctx->curKey, "uptime")) {
		Convert_ParseInt(val, &info->uptime);
	} else if (String_CaselessEqualsConst(&ctx->curKey, "mppass")) {
		String_Copy(&info->mppass, val);
	} else if (String_CaselessEqualsConst(&ctx->curKey, "ip")) {
		String_Copy(&info->ip, val);
	} else if (String_CaselessEqualsConst(&ctx->curKey, "port")) {
		Convert_ParseInt(val, &info->port);
	} else if (String_CaselessEqualsConst(&ctx->curKey, "software")) {
		String_Copy(&info->software, val);
	} else if (String_CaselessEqualsConst(&ctx->curKey, "featured")) {
		Convert_ParseBool(val, &info->featured);
	} else if (String_CaselessEqualsConst(&ctx->curKey, "country_abbr")) {
		/* Two letter country codes, see ISO 3166-1 alpha-2 */
		if (val->length < 2) return;

		/* classicube.net only works with lowercase flag urls */
		info->country[0] = val->buffer[0]; Char_MakeLower(info->country[0]);
		info->country[1] = val->buffer[1]; Char_MakeLower(info->country[1]);
	}
}

static void FetchServerTask_Handle(cc_uint8* data, cc_uint32 len) {
	curServer = &FetchServerTask.server;
	Json_Handle(data, len, ServerInfo_Parse, NULL, NULL);
}

void FetchServerTask_Run(const String* hash) {
	static const String id  = String_FromConst("CC fetch server");
	String url; char urlBuffer[URL_MAX_SIZE];
	if (FetchServerTask.Base.working) return;

	LWebTask_Reset(&FetchServerTask.Base);
	ServerInfo_Init(&FetchServerTask.server);
	String_InitArray(url, urlBuffer);
	String_Format1(&url, "http://dulm.blue/cube/server/fetch.php?s=%s", hash);

	FetchServerTask.Base.identifier = id;
	Http_AsyncGetDataEx(&url, false, &id, NULL, NULL, &ccCookies);
	FetchServerTask.Base.Handle  = FetchServerTask_Handle;
}


/*########################################################################################################################*
*-----------------------------------------------------FetchServersTask----------------------------------------------------*
*#########################################################################################################################*/
struct FetchServersData FetchServersTask;
static void FetchServersTask_Count(struct JsonContext* ctx) {
	FetchServersTask.numServers++;
}

static void FetchServersTask_Next(struct JsonContext* ctx) {
	curServer++;
	if (curServer < FetchServersTask.servers) return;
	ServerInfo_Init(curServer);
}

static void FetchServersTask_Handle(cc_uint8* data, cc_uint32 len) {
	int count;
	Mem_Free(FetchServersTask.servers);
	Mem_Free(FetchServersTask.orders);

	FetchServersTask.numServers = 0;
	FetchServersTask.servers    = NULL;
	FetchServersTask.orders     = NULL;

	/* -1 because servers is surrounded by a { */
	FetchServersTask.numServers = -1;
	Json_Handle(data, len, NULL, NULL, FetchServersTask_Count);
	count = FetchServersTask.numServers;

	if (count <= 0) return;
	FetchServersTask.servers = (struct ServerInfo*)Mem_Alloc(count, sizeof(struct ServerInfo), "servers list");
	FetchServersTask.orders  = (cc_uint16*)Mem_Alloc(count, 2, "servers order");

	/* -2 because servers is surrounded by a { */
	curServer = FetchServersTask.servers - 2;
	Json_Handle(data, len, ServerInfo_Parse, NULL, FetchServersTask_Next);
}

void FetchServersTask_Run(void) {
	static const String id  = String_FromConst("CC fetch servers");
	static const String url = String_FromConst("http://dulm.blue/cube/server/list.json");
	if (FetchServersTask.Base.working) return;
	LWebTask_Reset(&FetchServersTask.Base);

	FetchServersTask.Base.identifier = id;
	Http_AsyncGetDataEx(&url, false, &id, NULL, NULL, &ccCookies);
	FetchServersTask.Base.Handle = FetchServersTask_Handle;
}

void FetchServersTask_ResetOrder(void) {
	int i;
	for (i = 0; i < FetchServersTask.numServers; i++) {
		FetchServersTask.orders[i] = i;
	}
}


/*########################################################################################################################*
*-----------------------------------------------------CheckUpdateTask-----------------------------------------------------*
*#########################################################################################################################*/
struct CheckUpdateData CheckUpdateTask;
static char relVersionBuffer[16];

CC_NOINLINE static cc_uint64 CheckUpdateTask_ParseTime(const String* str) {
	String time, fractional;
	cc_uint64 secs;
	/* timestamp is in form of "seconds.fractional" */
	/* But only need to care about the seconds here */
	String_UNSAFE_Separate(str, '.', &time, &fractional);

	Convert_ParseUInt64(&time, &secs);
	return secs;
}

static void CheckUpdateTask_OnValue(struct JsonContext* ctx, const String* str) {
	if (String_CaselessEqualsConst(&ctx->curKey, "release_version")) {
		String_Copy(&CheckUpdateTask.latestRelease, str);
	} else if (String_CaselessEqualsConst(&ctx->curKey, "latest_ts")) {
		CheckUpdateTask.devTimestamp = CheckUpdateTask_ParseTime(str);
	} else if (String_CaselessEqualsConst(&ctx->curKey, "release_ts")) {
		CheckUpdateTask.relTimestamp = CheckUpdateTask_ParseTime(str);
	}
}

static void CheckUpdateTask_Handle(cc_uint8* data, cc_uint32 len) {
	Json_Handle(data, len, CheckUpdateTask_OnValue, NULL, NULL);
}

void CheckUpdateTask_Run(void) {
	static const String id  = String_FromConst("CC update check");
	static const String url = String_FromConst("http://cs.classicube.net/c_client/builds.json");
	if (CheckUpdateTask.Base.working) return;

	LWebTask_Reset(&CheckUpdateTask.Base);
	CheckUpdateTask.devTimestamp = 0;
	CheckUpdateTask.relTimestamp = 0;
	String_InitArray(CheckUpdateTask.latestRelease, relVersionBuffer);

	CheckUpdateTask.Base.identifier = id;
	Http_AsyncGetData(&url, false, &id);
	CheckUpdateTask.Base.Handle     = CheckUpdateTask_Handle;
}


/*########################################################################################################################*
*-----------------------------------------------------FetchUpdateTask-----------------------------------------------------*
*#########################################################################################################################*/
struct FetchUpdateData FetchUpdateTask;
static void FetchUpdateTask_Handle(cc_uint8* data, cc_uint32 len) {
	static const String path = String_FromConst(UPDATE_FILE);
	cc_result res;

	res = Stream_WriteAllTo(&path, data, len);
	if (res) { Logger_Warn(res, "saving update"); return; }

	res = Updater_SetNewBuildTime(FetchUpdateTask.timestamp);
	if (res) Logger_Warn(res, "setting update time");

	res = Updater_MarkExecutable();
	if (res) Logger_Warn(res, "making update executable");

#ifdef CC_BUILD_WIN
	Options_SetBool("update-dirty", true);
#endif
}

void FetchUpdateTask_Run(cc_bool release, cc_bool d3d9) {
	static char idBuffer[24];
	static int idCounter;
	String url; char urlBuffer[URL_MAX_SIZE];
	String_InitArray(url, urlBuffer);

	String_InitArray(FetchUpdateTask.Base.identifier, idBuffer);
	String_Format1(&FetchUpdateTask.Base.identifier, "CC update fetch%i", &idCounter);
	/* User may click another update button in the updates menu before original update finished downloading */
	/* Hence must use a different ID for each update fetch, otherwise old update gets downloaded and applied */
	idCounter++;

	String_Format2(&url, "http://cs.classicube.net/c_client/%c/%c",
		release ? "release"    : "latest",
		d3d9    ? Updater_D3D9 : Updater_OGL);
	if (FetchUpdateTask.Base.working) return;

	LWebTask_Reset(&FetchUpdateTask.Base);
	FetchUpdateTask.timestamp = release ? CheckUpdateTask.relTimestamp : CheckUpdateTask.devTimestamp;

	Http_AsyncGetData(&url, false, &FetchUpdateTask.Base.identifier);
	FetchUpdateTask.Base.Handle = FetchUpdateTask_Handle;
}


/*########################################################################################################################*
*-----------------------------------------------------FetchFlagsTask-----------------------------------------------------*
*#########################################################################################################################*/
struct FetchFlagsData FetchFlagsTask;
static int flagsCount, flagsCapacity;

struct Flag {
	Bitmap bmp;
	char country[2];
};
static struct Flag* flags;

/* Scales up flag bitmap if necessary */
static void FetchFlagsTask_Scale(Bitmap* bmp) {
	Bitmap scaled;
	int width  = Display_ScaleX(bmp->width);
	int height = Display_ScaleY(bmp->height);
	/* at default DPI don't need to rescale it */
	if (width == bmp->width && height == bmp->height) return;

	Bitmap_TryAllocate(&scaled, width, height);
	if (!scaled.scan0) {
		Logger_Warn(ERR_OUT_OF_MEMORY, "resizing flags bitmap"); return;
	}

	Bitmap_Scale(&scaled, bmp, 0, 0, bmp->width, bmp->height);
	Mem_Free(bmp->scan0);
	*bmp = scaled;
}

static void FetchFlagsTask_DownloadNext(void);
static void FetchFlagsTask_Handle(cc_uint8* data, cc_uint32 len) {
	struct Stream s;
	cc_result res;

	Stream_ReadonlyMemory(&s, data, len);
	res = Png_Decode(&flags[FetchFlagsTask.count].bmp, &s);
	if (res) Logger_Warn(res, "decoding flag");

	FetchFlagsTask_Scale(&flags[FetchFlagsTask.count].bmp);
	FetchFlagsTask.count++;
	FetchFlagsTask_DownloadNext();
}

static void FetchFlagsTask_DownloadNext(void) {
	static const String id = String_FromConst("CC get flag");
	String url; char urlBuffer[URL_MAX_SIZE];
	String_InitArray(url, urlBuffer);

	if (FetchFlagsTask.Base.working)        return;
	if (FetchFlagsTask.count == flagsCount) return;

	LWebTask_Reset(&FetchFlagsTask.Base);
	String_Format2(&url, "http://dulm.blue/cube/img/flags/%r%r.png",
			&flags[FetchFlagsTask.count].country[0], &flags[FetchFlagsTask.count].country[1]);

	FetchFlagsTask.Base.identifier = id;
	Http_AsyncGetData(&url, false, &id);
	FetchFlagsTask.Base.Handle = FetchFlagsTask_Handle;
}

static void FetchFlagsTask_Ensure(void) {
	if (flagsCount < flagsCapacity) return;
	flagsCapacity = flagsCount + 10;

	if (flags) {
		flags = (struct Flag*)Mem_Realloc(flags, flagsCapacity, sizeof(struct Flag), "flags");
	} else {
		flags = (struct Flag*)Mem_Alloc(flagsCapacity,          sizeof(struct Flag), "flags");
	}
}

void FetchFlagsTask_Add(const struct ServerInfo* server) {
	int i;
	for (i = 0; i < flagsCount; i++) {
		if (flags[i].country[0] != server->country[0]) continue;
		if (flags[i].country[1] != server->country[1]) continue;
		/* flag is already or will be downloaded */
		return;
	}
	FetchFlagsTask_Ensure();

	Bitmap_Init(flags[flagsCount].bmp, 0, 0, NULL);
	flags[flagsCount].country[0] = server->country[0];
	flags[flagsCount].country[1] = server->country[1];

	flagsCount++;
	FetchFlagsTask_DownloadNext();
}

Bitmap* Flags_Get(const struct ServerInfo* server) {
	int i;
	for (i = 0; i < FetchFlagsTask.count; i++) {
		if (flags[i].country[0] != server->country[0]) continue;
		if (flags[i].country[1] != server->country[1]) continue;
		return &flags[i].bmp;
	}
	return NULL;
}

void Flags_Free(void) {
	int i;
	for (i = 0; i < FetchFlagsTask.count; i++) {
		Mem_Free(flags[i].bmp.scan0);
	}
}
#endif
