#include "Resources.h"
#include "Funcs.h"
#include "String.h"
#include "Constants.h"
#include "Deflate.h"
#include "Stream.h"
#include "Platform.h"
#include "Launcher.h"
#include "Utils.h"
#include "Vorbis.h"
#include "Errors.h"
#include "Logger.h"
#include "LWeb.h"

#ifndef CC_BUILD_WEB
/*########################################################################################################################*
*--------------------------------------------------------Resources list---------------------------------------------------*
*#########################################################################################################################*/
static struct ResourceZip {
	const char* name;
	const char* url;
	short size;
	cc_bool downloaded;
} zipResource = { 
	"default zip", "http://dulm.blue/cube/dulmDefault.zip", 198 
};

static const char* const textureResources[20] = {
	{ "char.png"       }, { "clouds.png"      }, { "default.png" }, { "particles.png" },
	{ "rain.png"       }, { "gui_classic.png" }, { "icons.png"   }, { "terrain.png"   },
	{ "creeper.png"    }, { "pig.png"         }, { "sheep.png"   }, { "sheep_fur.png" },
	{ "skeleton.png"   }, { "spider.png"      }, { "zombie.png"  },
	{ "snow.png"       }, { "chicken.png"     }, { "gui.png"     },
	{ "animations.png" }, { "animations.txt"  }
};

static const char* const soundResources[59] = {
	{ "dig_cloth1"  }, { "dig_cloth2" },  { "dig_cloth3"  }, { "dig_cloth4", },
	{ "dig_grass1"  }, { "dig_grass2" },  { "dig_grass3"  }, { "dig_grass4" },
	{ "dig_gravel1" }, { "dig_gravel2" }, { "dig_gravel3" }, { "dig_gravel4" },
	{ "dig_sand1"   }, { "dig_sand2"  },  { "dig_sand3"   }, { "dig_sand4"  },
	{ "dig_snow1"   }, { "dig_snow2"  },  { "dig_snow3"   }, { "dig_snow4"  },
	{ "dig_stone1"  }, { "dig_stone2" },  { "dig_stone3"  }, { "dig_stone4" },
	{ "dig_wood1"   }, { "dig_wood2"  },  { "dig_wood3"   }, { "dig_wood4"  },
	{ "dig_glass1"  }, { "dig_glass2" },  { "dig_glass3"  },

	{ "step_cloth1"  }, { "step_cloth2"  }, { "step_cloth3"  }, { "step_cloth4"  },
	{ "step_grass1"  }, { "step_grass2"  }, { "step_grass3"  }, { "step_grass4"  },
	{ "step_gravel1" }, { "step_gravel2" }, { "step_gravel3" }, { "step_gravel4" },
	{ "step_sand1"   }, { "step_sand2"   }, { "step_sand3"   }, { "step_sand4"   },
	{ "step_snow1"   }, { "step_snow2"   }, { "step_snow3"   }, { "step_snow4"   },
	{ "step_stone1"  }, { "step_stone2"  }, { "step_stone3"  }, { "step_stone4"  },
	{ "step_wood1"   }, { "step_wood2"   }, { "step_wood3"   }, { "step_wood4"  }
};

static struct ResourceMusic { const char* name; short size; cc_bool downloaded; } musicResources[7] = {
	{ "calm1.ogg", 2472 }, { "calm2.ogg", 1931 }, { "calm3.ogg", 2181 },
	{ "hal1.ogg",  1926 }, { "hal2.ogg",  1714 }, { "hal3.ogg",  1879 }, { "hal4.ogg",  2499 }
};


/*########################################################################################################################*
*---------------------------------------------------------List/Checker----------------------------------------------------*
*#########################################################################################################################*/
int Resources_Count, Resources_Size;
static cc_bool allSoundsExist, allTexturesExist;
static int texturesFound;

CC_NOINLINE static const char* Resources_FindTex(const String* name) {
	const char* file;
	int i;

	for (i = 0; i < Array_Elems(textureResources); i++) {
		file = textureResources[i];
		if (String_CaselessEqualsConst(name, file)) return file;
	}
	return NULL;
}

static void Resources_CheckMusic(void) {
	String path; char pathBuffer[FILENAME_SIZE];
	int i;
	String_InitArray(path, pathBuffer);

	for (i = 0; i < Array_Elems(musicResources); i++) {
		path.length = 0;
		String_Format1(&path, "audio/%c", musicResources[i].name);

		musicResources[i].downloaded = File_Exists(&path);
		if (musicResources[i].downloaded) continue;

		Resources_Size += musicResources[i].size;
		Resources_Count++;
	}
}

static void Resources_CheckSounds(void) {
	String path; char pathBuffer[FILENAME_SIZE];
	int i;
	String_InitArray(path, pathBuffer);

	for (i = 0; i < Array_Elems(soundResources); i++) {
		path.length = 0;
		String_Format1(&path, "audio/%c.wav", soundResources[i]);

		if (File_Exists(&path)) continue;
		allSoundsExist = false;

		Resources_Count += Array_Elems(soundResources);
		Resources_Size  += 417;
		return;
	}
	allSoundsExist = true;
}

static cc_bool Resources_SelectZipEntry(const String* path) {
	String name = *path;
	Utils_UNSAFE_GetFilename(&name);

	if (Resources_FindTex(&name)) texturesFound++;
	return false;
}

static void Resources_CheckTextures(void) {
	static const String path = String_FromConst("texpacks/default.zip");
	struct Stream stream;
	struct ZipState state;
	cc_result res;

	res = Stream_OpenFile(&stream, &path);
	if (res == ReturnCode_FileNotFound) return;

	if (res) { Logger_Warn(res, "checking default.zip"); return; }
	Zip_Init(&state, &stream);
	state.SelectEntry = Resources_SelectZipEntry;

	res = Zip_Extract(&state);
	stream.Close(&stream);
	if (res) Logger_Warn(res, "inspecting default.zip");

	/* if somehow have say "gui.png", "GUI.png" */
	allTexturesExist = texturesFound >= Array_Elems(textureResources);
}

void Resources_CheckExistence(void) {
	Resources_CheckTextures();
	Resources_CheckMusic();
	Resources_CheckSounds();

	if (allTexturesExist) return;
	Resources_Count++;
	Resources_Size += zipResource.size;
}


/*########################################################################################################################*
*--------------------------------------------------------Audio patcher----------------------------------------------------*
*#########################################################################################################################*/
static void SoundPatcher_Save(const char* name, struct HttpRequest* req) {
	String path; char pathBuffer[STRING_SIZE];
	cc_result res;

	String_InitArray(path, pathBuffer);
	String_Format1(&path, "audio/%c.wav", name);

	res = Stream_WriteAllTo(&path, req->data, req->size);
	if (res) Logger_Warn(res, "saving sound file");
}

static void MusicPatcher_Save(const char* name, struct HttpRequest* req) {
	String path; char pathBuffer[STRING_SIZE];
	cc_result res;

	String_InitArray(path, pathBuffer);
	String_Format1(&path, "audio/%c", name);

	res = Stream_WriteAllTo(&path, req->data, req->size);
	if (res) Logger_Warn(res, "saving music file");
}


/*########################################################################################################################*
*-----------------------------------------------------------Fetcher-------------------------------------------------------*
*#########################################################################################################################*/
cc_bool Fetcher_Working, Fetcher_Completed, Fetcher_Failed;
int  Fetcher_StatusCode, Fetcher_Downloaded;
cc_result Fetcher_Result;

CC_NOINLINE static void Fetcher_DownloadMusic(const char* name) {
	String url; char urlBuffer[URL_MAX_SIZE];
	String id = String_FromReadonly(name);

	String_InitArray(url, urlBuffer);
	String_Format1(&url, "http://dulm.blue/cube/audio/%c", name);
	Http_AsyncGetData(&url, false, &id);
}

CC_NOINLINE static void Fetcher_DownloadSound(const char* name) {
	String url; char urlBuffer[URL_MAX_SIZE];
	String id = String_FromReadonly(name);

	String_InitArray(url, urlBuffer);
	String_Format1(&url, "http://dulm.blue/cube/audio/%c.wav", name);
	Http_AsyncGetData(&url, false, &id);
}

void Fetcher_Run(void) {
	String id, url;
	int i;
	if (Fetcher_Working) return;

	Fetcher_Failed     = false;
	Fetcher_Downloaded = 0;
	Fetcher_Working    = true;
	Fetcher_Completed  = false;

	if (!allTexturesExist) {
		id  = String_FromReadonly(zipResource.name);
		url = String_FromReadonly(zipResource.url);
		Http_AsyncGetData(&url, false, &id);
	}

	for (i = 0; i < Array_Elems(musicResources); i++) {
		if (musicResources[i].downloaded) continue;
		Fetcher_DownloadMusic(musicResources[i].name);
	}
	for (i = 0; i < Array_Elems(soundResources); i++) {
		if (allSoundsExist) continue;
		Fetcher_DownloadSound(soundResources[i]);
	}
}

static void Fetcher_Finish(void) {
	Fetcher_Completed = true;
	Fetcher_Working   = false;
}

CC_NOINLINE static cc_bool Fetcher_Get(const String* id, struct HttpRequest* req) {
	if (!Http_GetResult(id, req)) return false;

	if (req->success) {
		Fetcher_Downloaded++;
		return true;
	}

	Fetcher_Failed     = true;
	Fetcher_Result     = req->result;
	Fetcher_StatusCode = req->statusCode;

	HttpRequest_Free(req);
	Fetcher_Finish();
	return false;
}

static void Fetcher_CheckZip(void) {
	static String path = String_FromConst("texpacks/default.zip");
	struct HttpRequest req;
	cc_result res;
	String id;
	
	id = String_FromReadonly(zipResource.name);
	if (!Fetcher_Get(&id, &req)) return;
	zipResource.downloaded = true;

	res = Stream_WriteAllTo(&path, req.data, req.size);
	if (res) Logger_Warn(res, "saving music file");
	HttpRequest_Free(&req);
}

static void Fetcher_CheckMusic(struct ResourceMusic* music) {
	String id = String_FromReadonly(music->name);
	struct HttpRequest req;
	if (!Fetcher_Get(&id, &req)) return;

	music->downloaded = true;
	MusicPatcher_Save(music->name, &req);
	HttpRequest_Free(&req);
}

static void Fetcher_CheckSound(const char* name) {
	String id = String_FromReadonly(name);
	struct HttpRequest req;
	if (!Fetcher_Get(&id, &req)) return;

	SoundPatcher_Save(name, &req);
	HttpRequest_Free(&req);
}

/* TODO: Implement this.. */
/* TODO: How expensive is it to constantly do 'Get' over and make all these strings */
void Fetcher_Update(void) {
	int i;
	if (!zipResource.downloaded) Fetcher_CheckZip();

	for (i = 0; i < Array_Elems(musicResources); i++) {
		if (musicResources[i].downloaded) continue;
		Fetcher_CheckMusic(&musicResources[i]);
	}

	for (i = 0; i < Array_Elems(soundResources); i++) {
		Fetcher_CheckSound(soundResources[i]);
	}

	if (Fetcher_Downloaded != Resources_Count) return; 
	Fetcher_Finish();
}
#endif
