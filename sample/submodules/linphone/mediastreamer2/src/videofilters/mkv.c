#define bool_t ms_bool_t
#include "mediastreamer2/msfilter.h"
#include "mediastreamer2/msvideo.h"
#include "mediastreamer2/rfc3984.h"
#include "mediastreamer2/msticker.h"
#include "../audiofilters/waveheader.h"
#include "mediastreamer2/formats.h"
#undef bool_t

#define bool_t matroska_bool_t
#include <matroska/matroska.h>
#include <matroska/matroska_sem.h>
#undef bool_t

#define bool_t ambigous use ms_bool_t or matroska_bool_t

/*********************************************************************************************
 * Module interface                                                                          *
 *********************************************************************************************/
typedef void *(*ModuleNewFunc)();
typedef void (*ModuleFreeFunc)(void *obj);
typedef void (*ModuleSetFunc)(void *obj, const MSFmtDescriptor *fmt);
typedef void (*ModulePreProFunc)(void *obj, MSQueue *input, MSQueue *output);
typedef mblk_t *(*ModuleProFunc)(void *obj, mblk_t *buffer, ms_bool_t *isKeyFrame);
typedef void (*ModuleReverseFunc)(void *obj, mblk_t *input, MSQueue *output, ms_bool_t isFirstFrame);
typedef void (*ModulePrivateDataFunc)(const void *obj, uint8_t **data, size_t *data_size);
typedef void (*ModulePrivateDataLoadFunc)(void *obj, const uint8_t *data, size_t size);
typedef ms_bool_t (*ModuleIsKeyFrameFunc)(const mblk_t *frame);

typedef struct {
	const char *rfcName;
	const char *codecId;
	ModuleNewFunc new_module;
	ModuleFreeFunc free_module;
	ModuleSetFunc set;
	ModulePreProFunc preprocess;
	ModuleProFunc process;
	ModuleReverseFunc reverse;
	ModulePrivateDataFunc get_private_data;
	ModulePrivateDataLoadFunc load_private_data;
	ModuleIsKeyFrameFunc is_key_frame;
} ModuleDesc;

/*********************************************************************************************
 * h264 module                                                                               *
 *********************************************************************************************/
/* H264Private */
typedef struct {
	uint8_t profile;
	uint8_t level;
	uint8_t NALULenghtSizeMinusOne;
	MSList *sps_list;
	MSList *pps_list;
} H264Private;

static void H264Private_init(H264Private *obj) {
	obj->NALULenghtSizeMinusOne = 0xFF;
	obj->pps_list = NULL;
	obj->sps_list = NULL;
}

static inline ms_bool_t mblk_is_equal(mblk_t *b1, mblk_t *b2) {
	return msgdsize(b1) == msgdsize(b2) && memcmp(b1->b_rptr, b2->b_rptr, msgdsize(b1)) == 0;
}

static void H264Private_addSPS(H264Private *obj, mblk_t *sps) {
	MSList *element;
	for(element = obj->sps_list; element != NULL && mblk_is_equal((mblk_t *)element->data, sps); element = element->next);
	if(element != NULL || obj->sps_list == NULL) {
		obj->profile = sps->b_rptr[1];
		obj->level = sps->b_rptr[3];
		obj->sps_list = ms_list_append(obj->sps_list, sps);
	}
}

static void H264Private_addPPS(H264Private *obj, mblk_t *pps) {
	MSList *element;
	for(element = obj->pps_list; element != NULL && mblk_is_equal((mblk_t *)element->data, pps); element = element->next);
	if(element != NULL || obj->pps_list == NULL) {
		obj->pps_list = ms_list_append(obj->pps_list, pps);
	}
}

static inline const mblk_t *H264Private_getSPS(const H264Private *obj, int index) {
	return (mblk_t *)ms_list_nth_data(obj->sps_list, index);
}

static inline const mblk_t *H264Private_getPPS(const H264Private *obj, int index) {
	return (mblk_t *)ms_list_nth_data(obj->pps_list, index);
}

static void H264Private_serialize(const H264Private *obj, uint8_t **data, size_t *size) {
	*size = 7;

	uint8_t nbSPS = ms_list_size(obj->sps_list);
	uint8_t nbPPS = ms_list_size(obj->pps_list);

	*size += (nbSPS + nbPPS) * 2;

	MSList *it;
	for(it=obj->sps_list;it!=NULL;it=it->next) {
		mblk_t *buff = (mblk_t*)(it->data);
		*size += msgdsize(buff);
	}
	for(it=obj->pps_list;it!=NULL;it=it->next) {
		mblk_t *buff = (mblk_t*)(it->data);
		*size += msgdsize(buff);
	}

	uint8_t *result = (uint8_t *)ms_new0(uint8_t, *size);
	result[0] = 0x01;
	result[1] = obj->profile;
	result[3] = obj->level;
	result[4] = obj->NALULenghtSizeMinusOne;
	result[5] = nbSPS & 0x1F;

	int i=6;
	for(it=obj->sps_list; it!=NULL; it=it->next) {
		mblk_t *buff = (mblk_t*)(it->data);
		size_t buff_size = msgdsize(buff);
		uint16_t buff_size_be = htons(buff_size);
		memcpy(&result[i], &buff_size_be, sizeof(buff_size_be));
		i+=sizeof(buff_size_be);
		memcpy(&result[i], buff->b_rptr, buff_size);
		i += buff_size;
	}

	result[i] = nbPPS;
	i++;

	for(it=obj->pps_list; it!=NULL; it=it->next) {
		mblk_t *buff = (mblk_t*)(it->data);
		int buff_size = msgdsize(buff);
		uint16_t buff_size_be = htons(buff_size);
		memcpy(&result[i], &buff_size_be, sizeof(buff_size_be));
		i+=sizeof(buff_size_be);
		memcpy(&result[i], buff->b_rptr, buff_size);
		i += buff_size;
	}
	*data = result;
}

static void H264Private_load(H264Private *obj, const uint8_t *data) {
	int i;

	obj->profile = data[1];
	obj->level = data[3];
	obj->NALULenghtSizeMinusOne = data[4];

	int nbSPS = data[5] & 0x1F;
	const uint8_t *r_ptr = data + 6;
	for(i=0;i<nbSPS;i++) {
		uint16_t nalu_size;
		memcpy(&nalu_size, r_ptr, sizeof(uint16_t)); r_ptr += sizeof(uint16_t);
		nalu_size = ntohs(nalu_size);
		mblk_t *nalu = allocb(nalu_size, 0);
		memcpy(nalu->b_wptr, r_ptr, nalu_size); nalu->b_wptr += nalu_size; r_ptr += nalu_size;
		obj->sps_list = ms_list_append(obj->sps_list, nalu);
	}

	int nbPPS = *r_ptr; r_ptr += 1;
	for(i=0;i<nbPPS;i++) {
		uint16_t nalu_size;
		memcpy(&nalu_size, r_ptr, sizeof(uint16_t)); r_ptr += sizeof(uint16_t);
		nalu_size = ntohs(nalu_size);
		mblk_t *nalu = allocb(nalu_size, 0);
		memcpy(nalu->b_wptr, r_ptr, nalu_size); nalu->b_wptr += nalu_size; r_ptr += nalu_size;
		obj->pps_list = ms_list_append(obj->pps_list, nalu);
	}
}

static void H264Private_uninit(H264Private *obj) {
	ms_list_free_with_data(obj->sps_list,(void (*)(void *))freemsg);
	ms_list_free_with_data(obj->pps_list,(void (*)(void *))freemsg);
}

/* h264 module */
typedef struct {
	Rfc3984Context rfc3984Context;
	H264Private codecPrivate;
} H264Module;

static void *h264_module_new() {
	H264Module *mod = ms_new0(H264Module, 1);
	rfc3984_init(&mod->rfc3984Context);
	H264Private_init(&mod->codecPrivate);
	return mod;
}

static void h264_module_free(void *data) {
	H264Module *obj = (H264Module *)data;
	rfc3984_uninit(&obj->rfc3984Context);
	H264Private_uninit(&obj->codecPrivate);
	ms_free(obj);
}

static void h264_module_preprocessing(void *data, MSQueue *input, MSQueue *output) {
	H264Module *obj = (H264Module *)data;
	MSQueue queue;
	mblk_t *inputBuffer;

	ms_queue_init(&queue);
	while((inputBuffer = ms_queue_get(input)) != NULL) {
		rfc3984_unpack(&obj->rfc3984Context, inputBuffer, &queue);
		if(!ms_queue_empty(&queue)) {
			mblk_t *frame = ms_queue_get(&queue);
			mblk_t *end = frame;
			while(!ms_queue_empty(&queue)) {
				end = concatb(end, ms_queue_get(&queue));
			}
			ms_queue_put(output, frame);
		}
	}
}

static inline int h264_nalu_type(const mblk_t *nalu) {
	return (nalu->b_rptr[0]) & ((1<<5)-1);
}

static ms_bool_t h264_is_key_frame(const mblk_t *frame) {
	const mblk_t *curNalu;
	for(curNalu = frame; curNalu != NULL && h264_nalu_type(curNalu) != 5; curNalu = curNalu->b_cont);
	return curNalu != NULL;
}

static void nalus_to_frame(mblk_t *buffer, mblk_t **frame, mblk_t **sps, mblk_t **pps, ms_bool_t *isKeyFrame) {
	mblk_t *curNalu;
	*frame = NULL;
	*sps = NULL;
	*pps = NULL;
	*isKeyFrame = FALSE;
	uint32_t timecode = mblk_get_timestamp_info(buffer);

	for(curNalu = buffer; curNalu != NULL;) {
		mblk_t *buff = curNalu;
		curNalu = curNalu->b_cont;
		buff->b_cont = NULL;

		int type = h264_nalu_type(buff);
		switch(h264_nalu_type(buff)) {
		case 7:
			if(*sps == NULL) {
				*sps = buff;
			} else {
				concatb(*sps, buff);
			}
			break;

		case 8:
			if(*pps == NULL) {
				*pps = buff;
			} else {
				concatb(*pps, buff);
			}
			break;

		default:
			if(type == 5) {
				*isKeyFrame = TRUE;
			}
			uint32_t bufferSize = htonl(msgdsize(buff));
			mblk_t *size = allocb(4, 0);
			memcpy(size->b_wptr, &bufferSize, sizeof(bufferSize));
			size->b_wptr = size->b_wptr + sizeof(bufferSize);
			concatb(size, buff);
			buff = size;

			if(*frame == NULL) {
				*frame = buff;
			} else {
				concatb(*frame, buff);
			}
		}
	}

	if(*frame != NULL) {
		msgpullup(*frame, -1);
		mblk_set_timestamp_info(*frame, timecode);
	}
	if(*sps != NULL) {
		msgpullup(*sps, -1);
		mblk_set_timestamp_info(*sps, timecode);
	}
	if(*pps != NULL) {
		msgpullup(*pps, -1);
		mblk_set_timestamp_info(*pps, timecode);
	}
}

static mblk_t *h264_module_processing(void *data, mblk_t *nalus, ms_bool_t *isKeyFrame) {
	H264Module *obj = (H264Module *)data;
	mblk_t *frame, *sps=NULL, *pps=NULL;
	nalus_to_frame(nalus, &frame, &sps, &pps, isKeyFrame);
	if(sps != NULL) {
		H264Private_addSPS(&obj->codecPrivate, sps);
	}
	if(pps != NULL) {
		H264Private_addPPS(&obj->codecPrivate, pps);
	}
	return frame;
}

static void h264_module_reverse(void *data, mblk_t *input, MSQueue *output, ms_bool_t isFirstFrame) {
	mblk_t *buffer = NULL, *bufferFrag;
	H264Module *obj = (H264Module *)data;
	MSQueue queue;
	ms_queue_init(&queue);
	while(input->b_rptr != input->b_wptr) {
		uint32_t naluSize;
		mblk_t *nalu;
		memcpy(&naluSize, input->b_rptr, sizeof(uint32_t)); input->b_rptr += sizeof(uint32_t);
		naluSize = ntohl(naluSize);
		nalu = allocb(naluSize, 0);
		memcpy(nalu->b_wptr, input->b_rptr, naluSize); nalu->b_wptr += naluSize; input->b_rptr += naluSize;
		if(buffer == NULL) {
			buffer = nalu;
		} else {
			concatb(buffer, nalu);
		}
	}
	freemsg(input);
	if(h264_is_key_frame(buffer)) {
		ms_queue_put(&queue, copymsg(H264Private_getSPS(&obj->codecPrivate, 0)));
		ms_queue_put(&queue, copymsg(H264Private_getPPS(&obj->codecPrivate, 0)));
	}
	for(bufferFrag = buffer; bufferFrag != NULL;) {
		mblk_t *curBuff = bufferFrag;
		bufferFrag = bufferFrag->b_cont;
		curBuff->b_cont = NULL;
		ms_queue_put(&queue, curBuff);
	}
	rfc3984_pack(&obj->rfc3984Context, &queue, output, mblk_get_timestamp_info(input));
}

static void h264_module_get_private_data(const void *o, uint8_t **data, size_t *data_size) {
	const H264Module *obj = (const H264Module *)o;
	H264Private_serialize(&obj->codecPrivate, data, data_size);
}

static void h264_module_load_private_data(void *o, const uint8_t *data, size_t size) {
	H264Module *obj = (H264Module *)o;
	H264Private_load(&obj->codecPrivate, data);
}

/* h264 module description */
#ifdef _MSC_VER
static const ModuleDesc h264_module_desc = {
	"H264",
	"V_MPEG4/ISO/AVC",
	h264_module_new,
	h264_module_free,
	NULL,
	h264_module_preprocessing,
	h264_module_processing,
	h264_module_reverse,
	h264_module_get_private_data,
	h264_module_load_private_data,
	h264_is_key_frame
};
#else
static const ModuleDesc h264_module_desc = {
	.rfcName = "H264",
	.codecId = "V_MPEG4/ISO/AVC",
	.new_module = h264_module_new,
	.free_module = h264_module_free,
	.set = NULL,
	.preprocess = h264_module_preprocessing,
	.process = h264_module_processing,
	.reverse = h264_module_reverse,
	.get_private_data = h264_module_get_private_data,
	.load_private_data = h264_module_load_private_data,
	.is_key_frame = h264_is_key_frame
};
#endif

/*********************************************************************************************
 * µLaw module                                                                               *
 *********************************************************************************************/
/* WavPrivate */
typedef struct {
	uint16_t wFormatTag;
	uint16_t nbChannels;
	uint32_t nSamplesPerSec;
	uint32_t nAvgBytesPerSec;
	uint16_t nBlockAlign;
	uint16_t wBitsPerSample;
	uint16_t cbSize;
} WavPrivate;

static void wav_private_set(WavPrivate *data, const MSFmtDescriptor *obj) {
	uint16_t bitsPerSample = 8;
	uint16_t nbBlockAlign = (bitsPerSample * obj->nchannels)/8;
	uint32_t bitrate = bitsPerSample * obj->nchannels * obj->rate;

	data->wFormatTag = le_uint16((uint16_t)7);
	data->nbChannels = le_uint16((uint16_t)obj->nchannels);
	data->nSamplesPerSec = le_uint32((uint32_t)obj->rate);
	data->nAvgBytesPerSec = le_uint32(bitrate);
	data->nBlockAlign = le_uint16((uint16_t)nbBlockAlign);
	data->wBitsPerSample = le_uint16(bitsPerSample);
	data->cbSize = 0;
}

static void wav_private_serialize(const WavPrivate *obj, uint8_t **data, size_t *size) {
	*size = 22;
	*data = (uint8_t *)ms_new0(uint8_t, *size);
	memcpy(*data, obj, *size);
}

static inline void wav_private_load(WavPrivate *obj, const uint8_t *data) {
	memcpy(obj, data, 22);
}

/* µLaw module */
static void *mu_law_module_new() {
	return ms_new0(WavPrivate, 1);
}

static void mu_law_module_free(void *o) {
	ms_free(o);
}

static void mu_law_module_set(void *o, const MSFmtDescriptor *fmt) {
	WavPrivate *obj = (WavPrivate *)o;
	wav_private_set(obj, fmt);
}

static void mu_law_module_get_private_data(const void *o, uint8_t **data, size_t *data_size) {
	const WavPrivate *obj = (const WavPrivate *)o;
	wav_private_serialize(obj, data, data_size);
}

static void mu_law_module_load_private(void *o, const uint8_t *data, size_t size) {
	WavPrivate *obj = (WavPrivate *)o;
	wav_private_load(obj, data);
}

/* µLaw module description */
#ifdef _MSC_VER
static const ModuleDesc mu_law_module_desc = {
	"pcmu",
	"A_MS/ACM",
	mu_law_module_new,
	mu_law_module_free,
	mu_law_module_set,
	NULL,
	NULL,
	NULL,
	mu_law_module_get_private_data,
	mu_law_module_load_private,
	NULL
};
#else
static const ModuleDesc mu_law_module_desc = {
	.rfcName = "pcmu",
	.codecId = "A_MS/ACM",
	.new_module = mu_law_module_new,
	.free_module = mu_law_module_free,
	.set = mu_law_module_set,
	.preprocess = NULL,
	.process = NULL,
	.reverse = NULL,
	.get_private_data = mu_law_module_get_private_data,
	.load_private_data = mu_law_module_load_private,
	.is_key_frame = NULL
};
#endif

/*********************************************************************************************
 * Opus module                                                                               *
 *********************************************************************************************/
// OpusCodecPrivate
typedef struct {
	uint8_t version;
	uint8_t channelCount;
	uint16_t preSkip;
	uint32_t inputSampleRate;
	uint16_t outputGain;
	uint8_t mappingFamily;
} OpusCodecPrivate;

static void opus_codec_private_init(OpusCodecPrivate *obj) {
	memset(obj, 0, sizeof(OpusCodecPrivate));
	obj->version = 1;
	obj->preSkip = le_uint16(3840); // 80ms at 48kHz
	obj->outputGain = le_uint16(0);
}

static void opus_codec_private_set(OpusCodecPrivate *obj, int nChannels, int inputSampleRate) {
	obj->channelCount = nChannels;
	obj->inputSampleRate = le_uint32((uint32_t)inputSampleRate);
}

static void opus_codec_private_serialize(const OpusCodecPrivate *obj, uint8_t **data, size_t *size) {
	const char signature[9] = "OpusHead";
	*size = 19;
	*data = ms_new0(uint8_t, *size);
	memcpy(*data, signature, 8);
	memcpy((*data)+8, obj, 11);
}

static inline void opus_codec_private_load(OpusCodecPrivate *obj, const uint8_t *data, size_t size) {
	memcpy(obj, data+8, 11);
}

// OpusModule
static void *opus_module_new() {
	OpusCodecPrivate *obj = ms_new(OpusCodecPrivate, 1);
	opus_codec_private_init(obj);
	return obj;
}

static void opus_module_free(void *o) {
	OpusCodecPrivate *obj = (OpusCodecPrivate *)o;
	ms_free(obj);
}

static void opus_module_set(void *o, const MSFmtDescriptor *fmt) {
	OpusCodecPrivate *obj = (OpusCodecPrivate *)o;
	opus_codec_private_set(obj, fmt->nchannels, 0);
}

static void opus_module_get_private_data(const void *o, uint8_t **data, size_t *dataSize) {
	const OpusCodecPrivate *obj = (const OpusCodecPrivate *)o;
	opus_codec_private_serialize(obj, data, dataSize);
}

static void opus_module_load_private_data(void *o, const uint8_t *data, size_t size) {
	OpusCodecPrivate *obj = (OpusCodecPrivate *)o;
	opus_codec_private_load(obj, data, size);
}

#ifdef _MSC_VER
static const ModuleDesc opus_module_desc = {
	"opus",
	"A_OPUS",
	opus_module_new,
	opus_module_free,
	opus_module_set,
	NULL,
	NULL,
	NULL,
	opus_module_get_private_data,
	opus_module_load_private_data,
	NULL
};
#else
static const ModuleDesc opus_module_desc = {
	.rfcName = "opus",
	.codecId = "A_OPUS",
	.new_module = opus_module_new,
	.free_module = opus_module_free,
	.set = opus_module_set,
	.preprocess = NULL,
	.process = NULL,
	.reverse = NULL,
	.get_private_data = opus_module_get_private_data,
	.load_private_data = opus_module_load_private_data,
	.is_key_frame = NULL
};
#endif

/*********************************************************************************************
 * Modules list                                                                              *
 *********************************************************************************************/
typedef enum {
	H264_MOD_ID,
	MU_LAW_MOD_ID,
	OPUS_MOD_ID,
	NONE_ID
} ModuleId;

static const ModuleDesc const *moduleDescs[] = {
	&h264_module_desc,
	&mu_law_module_desc,
	&opus_module_desc,
	NULL
};

static int find_module_id_from_rfc_name(const char *rfcName) {
	int id;
	for(id=0; moduleDescs[id] != NULL && strcmp(moduleDescs[id]->rfcName, rfcName) != 0; id++);
	return id;
}

static int find_module_id_from_codec_id(const char *codecId) {
	int id;
	for(id=0; moduleDescs[id] != NULL && strcmp(moduleDescs[id]->codecId, codecId) != 0; id++);
	return id;
}

static const char *codec_id_to_rfc_name(const char *codecId) {
	ModuleId id = find_module_id_from_codec_id(codecId);
	if(id == NONE_ID) {
		return NULL;
	} else {
		return moduleDescs[id]->rfcName;
	}
}

/*********************************************************************************************
 * Module                                                                                    *
 *********************************************************************************************/
typedef struct {
	ModuleId id;
	void *data;
} Module;

static Module *module_new(const char *rfcName) {
	ModuleId id = find_module_id_from_rfc_name(rfcName);
	if(id == NONE_ID) {
		return NULL;
	} else {
		Module *module = (Module *)ms_new0(Module, 1);
		module->id = id;
		module->data = moduleDescs[module->id]->new_module();
		return module;
	}
}

static void module_free(Module *module) {
	moduleDescs[module->id]->free_module(module->data);
	ms_free(module);
}

static void module_set(Module *module, const MSFmtDescriptor *format) {
	if(moduleDescs[module->id]->set != NULL) {
		moduleDescs[module->id]->set(module->data, format);
	}
}

static void module_preprocess(Module *module, MSQueue *input, MSQueue *output) {
	if(moduleDescs[module->id]->preprocess != NULL) {
		moduleDescs[module->id]->preprocess(module->data, input, output);
	} else {
		mblk_t *buffer;
		while((buffer = ms_queue_get(input)) != NULL) {
			ms_queue_put(output, buffer);
		}
	}
}

static mblk_t *module_process(Module *module, mblk_t *buffer, ms_bool_t *isKeyFrame) {
	mblk_t *frame;
	if(moduleDescs[module->id]->process != NULL) {
		frame = moduleDescs[module->id]->process(module->data, buffer, isKeyFrame);
	} else {
		frame = buffer;
		*isKeyFrame = TRUE;
	}
	return frame;
}

static void module_reverse(Module *module, mblk_t *input, MSQueue *output, ms_bool_t isFirstFrame) {
	if(moduleDescs[module->id]->reverse == NULL) {
		ms_queue_put(output, input);
	} else {
		moduleDescs[module->id]->reverse(module->data, input, output, isFirstFrame);
	}
}

static inline void module_get_private_data(const Module *module, uint8_t **data, size_t *dataSize) {
	moduleDescs[module->id]->get_private_data(module->data, data, dataSize);
}

static inline void module_load_private_data(Module *module, const uint8_t *data, size_t size) {
	moduleDescs[module->id]->load_private_data(module->data, data, size);
}

static inline ms_bool_t module_is_key_frame(const Module *module, const mblk_t *frame) {
	return moduleDescs[module->id]->is_key_frame(frame);
}

static inline const char *module_get_codec_id(const Module *module) {
	return moduleDescs[module->id]->codecId;
}

/*********************************************************************************************
 * Matroska                                                                                  *
 *********************************************************************************************/
#define WRITE_DEFAULT_ELEMENT TRUE

static const timecode_t MKV_TIMECODE_SCALE = 1000000;
static const int MKV_DOCTYPE_VERSION = 2;
static const int MKV_DOCTYPE_READ_VERSION = 2;

extern const nodemeta LangStr_Class[];
extern const nodemeta UrlPart_Class[];
extern const nodemeta BufStream_Class[];
extern const nodemeta MemStream_Class[];
extern const nodemeta Streams_Class[];
extern const nodemeta File_Class[];
extern const nodemeta Stdio_Class[];
extern const nodemeta Matroska_Class[];
extern const nodemeta EBMLElement_Class[];
extern const nodemeta EBMLMaster_Class[];
extern const nodemeta EBMLBinary_Class[];
extern const nodemeta EBMLString_Class[];
extern const nodemeta EBMLInteger_Class[];
extern const nodemeta EBMLCRC_Class[];
extern const nodemeta EBMLDate_Class[];
extern const nodemeta EBMLVoid_Class[];

static void loadModules(nodemodule *modules) {
	NodeRegisterClassEx(modules, Streams_Class);
	NodeRegisterClassEx(modules, File_Class);
	NodeRegisterClassEx(modules, Matroska_Class);
	NodeRegisterClassEx(modules, EBMLElement_Class);
	NodeRegisterClassEx(modules, EBMLMaster_Class);
	NodeRegisterClassEx(modules, EBMLBinary_Class);
	NodeRegisterClassEx(modules, EBMLString_Class);
	NodeRegisterClassEx(modules, EBMLInteger_Class);
	NodeRegisterClassEx(modules, EBMLCRC_Class);
	NodeRegisterClassEx(modules, EBMLDate_Class);
	NodeRegisterClassEx(modules, EBMLVoid_Class);
}

typedef enum {
	MKV_OPEN_CREATE,
	MKV_OPEN_APPEND,
	MKV_OPEN_RO
} MatroskaOpenMode;

typedef struct {
	parsercontext *p;
	stream *output;
	ebml_master *header, *segment, *cluster, *info, *tracks, *metaSeek, *cues, *firstCluster, *currentCluster;
	matroska_seekpoint *infoMeta, *tracksMeta, *cuesMeta;
	matroska_block *currentBlock;
	timecode_t timecodeScale;
	filepos_t segmentInfoPosition;
	int nbClusters;
} Matroska;

static void matroska_init(Matroska *obj) {
	memset(obj, 0, sizeof(Matroska));
	obj->p = (parsercontext *)ms_new0(parsercontext, 1);
	ParserContext_Init(obj->p, NULL, NULL, NULL);
	loadModules((nodemodule*)obj->p);
	MATROSKA_Init((nodecontext*)obj->p);
	obj->segmentInfoPosition = -1;
	obj->timecodeScale = -1;
}

static void matroska_uninit(Matroska *obj) {
	MATROSKA_Done((nodecontext*)obj->p);
	ParserContext_Done(obj->p);
	ms_free(obj->p);
}

static int ebml_reading_profile(const ebml_master *head) {
	size_t length = EBML_ElementDataSize((ebml_element *)head, FALSE);
	char *docType = ms_new0(char, length);
	EBML_StringGet((ebml_string *)EBML_MasterFindChild(head, &EBML_ContextDocType), docType, length);
	int docTypeReadVersion = EBML_IntegerValue((ebml_integer *)EBML_MasterFindChild(head, &EBML_ContextDocTypeReadVersion));
	int profile;

	if(strcmp(docType, "matroska")==0) {
		switch (docTypeReadVersion) {
		case 1:
			profile = PROFILE_MATROSKA_V1;
			break;
		case 2:
			profile = PROFILE_MATROSKA_V2;
			break;
		case 3:
			profile = PROFILE_MATROSKA_V3;
			break;
		case 4:
			profile = PROFILE_MATROSKA_V4;
			break;
		default:
			profile = -1;
		}
	} else if(strcmp(docType, "webm")) {
		profile = PROFILE_WEBM;
	} else {
		profile = -1;
	}

	ms_free(docType);
	return profile;
}

static ms_bool_t matroska_create_file(Matroska *obj, const char path[]) {
	obj->header = (ebml_master *)EBML_ElementCreate(obj->p, &EBML_ContextHead, TRUE, NULL);
	obj->segment = (ebml_master *)EBML_ElementCreate(obj->p, &MATROSKA_ContextSegment, TRUE, NULL);
	obj->metaSeek = (ebml_master *)EBML_MasterAddElt(obj->segment, &MATROSKA_ContextSeekHead, FALSE);
	obj->infoMeta = (matroska_seekpoint *)EBML_MasterAddElt(obj->metaSeek, &MATROSKA_ContextSeek, TRUE);
	obj->tracksMeta = (matroska_seekpoint *)EBML_MasterAddElt(obj->metaSeek, &MATROSKA_ContextSeek, TRUE);
	obj->cuesMeta = (matroska_seekpoint *)EBML_MasterAddElt(obj->metaSeek, &MATROSKA_ContextSeek, TRUE);
	obj->info = (ebml_master *)EBML_MasterAddElt(obj->segment, &MATROSKA_ContextInfo, TRUE);
	obj->tracks = (ebml_master *)EBML_MasterAddElt(obj->segment, &MATROSKA_ContextTracks, FALSE);
	obj->cues = (ebml_master *)EBML_MasterAddElt(obj->segment, &MATROSKA_ContextCues, FALSE);
	obj->timecodeScale = MKV_TIMECODE_SCALE;

	MATROSKA_LinkMetaSeekElement(obj->infoMeta, (ebml_element *)obj->info);
	MATROSKA_LinkMetaSeekElement(obj->tracksMeta, (ebml_element *)obj->tracks);
	MATROSKA_LinkMetaSeekElement(obj->cuesMeta, (ebml_element *)obj->cues);

	return TRUE;
}

static ms_bool_t matroska_load_file(Matroska *obj) {
	int upperLevels = 0;
	ebml_parser_context readContext;
	readContext.Context = &MATROSKA_ContextStream;
	readContext.EndPosition = INVALID_FILEPOS_T;
	readContext.Profile = 0;
	readContext.UpContext = NULL;

	obj->header = (ebml_master *)EBML_FindNextElement(obj->output, &readContext, &upperLevels, FALSE);
	EBML_ElementReadData(obj->header, obj->output, &readContext, FALSE, SCOPE_ALL_DATA, 0);
	readContext.Profile = ebml_reading_profile((ebml_master *)obj->header);

	obj->segment = (ebml_master *)EBML_FindNextElement(obj->output, &readContext, &upperLevels, FALSE);
	EBML_ElementReadData(obj->segment, obj->output, &readContext, FALSE, SCOPE_PARTIAL_DATA, 0);

	ebml_element *elt;
	for(elt = EBML_MasterChildren(obj->segment); elt != NULL; elt = EBML_MasterNext(elt)) {
		if(EBML_ElementIsType(elt, &MATROSKA_ContextSeekHead)) {
			obj->metaSeek = (ebml_master*)elt;
			matroska_seekpoint *seekPoint;
			for(seekPoint = (matroska_seekpoint *)EBML_MasterChildren(obj->metaSeek); seekPoint != NULL; seekPoint = (matroska_seekpoint *)EBML_MasterNext(seekPoint)) {
				if(MATROSKA_MetaSeekIsClass(seekPoint, &MATROSKA_ContextInfo)) {
					obj->infoMeta = seekPoint;
				} else if(MATROSKA_MetaSeekIsClass(seekPoint, &MATROSKA_ContextTracks)) {
					obj->tracksMeta = seekPoint;
				} else if(MATROSKA_MetaSeekIsClass(seekPoint, &MATROSKA_ContextCues)) {
					obj->cuesMeta = seekPoint;
				}
			}
		} else if(EBML_ElementIsType(elt, &MATROSKA_ContextInfo)) {
			obj->info = (ebml_master*)elt;
			obj->timecodeScale = EBML_IntegerValue((ebml_integer *)EBML_MasterFindChild(obj->info, &MATROSKA_ContextTimecodeScale));
			MATROSKA_LinkMetaSeekElement(obj->infoMeta, (ebml_element *)obj->info);
		} else if(EBML_ElementIsType(elt, &MATROSKA_ContextTracks)) {
			obj->tracks = (ebml_master*)elt;
			MATROSKA_LinkMetaSeekElement(obj->tracksMeta, (ebml_element *)obj->tracks);
		} else if(EBML_ElementIsType(elt, &MATROSKA_ContextCues)) {
			obj->cues = (ebml_master*)elt;
			MATROSKA_LinkMetaSeekElement(obj->cuesMeta, (ebml_element *)obj->cues);
		} else if(EBML_ElementIsType(elt, &MATROSKA_ContextCluster)) {
			obj->cluster = (ebml_master *)elt;
			if(obj->nbClusters == 0) {
				obj->firstCluster = obj->cluster;
			}
			MATROSKA_LinkClusterBlocks((matroska_cluster *)obj->cluster, obj->segment, obj->tracks, FALSE);
			obj->nbClusters++;
		}
	}
	return TRUE;
}

static int matroska_open_file(Matroska *obj, const char path[], MatroskaOpenMode mode) {
	int err = 0;

	switch(mode) {
	case MKV_OPEN_CREATE:
		if((obj->output = StreamOpen(obj->p, path, SFLAG_WRONLY | SFLAG_CREATE)) == NULL) {
			err = -2;
			break;
		}
		if(!matroska_create_file(obj, path)) {
			err = -3;
		}
		break;

	case MKV_OPEN_APPEND:
		if((obj->output = StreamOpen(obj->p, path, SFLAG_REOPEN)) == NULL) {
			err = -2;
			break;
		}
		if(!matroska_load_file(obj)) {
			err = -3;
			break;
		}
		if(obj->cues == NULL) {
			obj->cues = (ebml_master *)EBML_ElementCreate(obj->p, &MATROSKA_ContextCues, FALSE, NULL);
		}
		if(obj->cluster == NULL) {
			Stream_Seek(obj->output, 0, SEEK_END);
		} else {
			Stream_Seek(obj->output, EBML_ElementPositionEnd((ebml_element *)obj->cluster), SEEK_SET);
		}
		break;

	case MKV_OPEN_RO:
		if((obj->output = StreamOpen(obj->p, path, SFLAG_RDONLY)) == NULL) {
			err = -2;
			break;
		}
		if(!matroska_load_file(obj)) {
			err = -3;
			break;
		}
		break;

	default:
		err = -1;
		break;
	}
	return err;
}

static void matroska_close_file(Matroska *obj) {
	if(obj->output != NULL) {
		StreamClose(obj->output);
	}
	if(obj->header != NULL) {
		Node_Release((node *)obj->header);
	}
	if(obj->segment != NULL) {
		Node_Release((node *)obj->segment);
	}
	obj->output = NULL;
	obj->header = NULL;
	obj->segment = NULL;
	obj->cluster = NULL;
	obj->info = NULL;
	obj->tracks = NULL;
	obj->metaSeek = NULL;
	obj->cues = NULL;
	obj->firstCluster = NULL;
	obj->currentCluster = NULL;
	obj->infoMeta = NULL;
	obj->tracksMeta = NULL;
	obj->cuesMeta = NULL;
	obj->currentBlock = NULL;
	obj->timecodeScale = -1;
	obj->segmentInfoPosition = -1;
	obj->nbClusters = 0;
}

static void matroska_set_doctype_version(Matroska *obj, int doctypeVersion, int doctypeReadVersion) {
	EBML_IntegerSetValue((ebml_integer*)EBML_MasterFindChild(obj->header, &EBML_ContextDocTypeVersion), doctypeVersion);
	EBML_IntegerSetValue((ebml_integer*)EBML_MasterFindChild(obj->header, &EBML_ContextDocTypeReadVersion), doctypeReadVersion);
}

static inline void matroska_write_ebml_header(Matroska *obj) {
	EBML_ElementRender((ebml_element *)obj->header, obj->output, WRITE_DEFAULT_ELEMENT, FALSE, FALSE, NULL);
}

static int matroska_set_segment_info(Matroska *obj, const char writingApp[], const char muxingApp[], double duration) {
	if(obj->timecodeScale == -1) {
		return -1;
	} else {
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild(obj->info, &MATROSKA_ContextTimecodeScale), obj->timecodeScale);
		EBML_StringSetValue((ebml_string*)EBML_MasterGetChild(obj->info, &MATROSKA_ContextMuxingApp), muxingApp);
		EBML_StringSetValue((ebml_string*)EBML_MasterGetChild(obj->info, &MATROSKA_ContextWritingApp), writingApp);
		EBML_FloatSetValue((ebml_float *)EBML_MasterGetChild(obj->info, &MATROSKA_ContextDuration), duration);
		return 0;
	}
}

static inline timecode_t matroska_get_duration(const Matroska *obj) {
	return (timecode_t)EBML_FloatValue((ebml_float *)EBML_MasterFindChild(obj->info, &MATROSKA_ContextDuration));
}

static void updateElementHeader(ebml_element *element, stream *file) {
	filepos_t initial_pos = Stream_Seek(file, 0, SEEK_CUR);
	Stream_Seek(file, EBML_ElementPosition(element), SEEK_SET);
	EBML_ElementUpdateSize(element, WRITE_DEFAULT_ELEMENT, FALSE);
	EBML_ElementRenderHead(element, file, FALSE, NULL);
	Stream_Seek(file, initial_pos, SEEK_SET);
}

static inline void matroska_start_segment(Matroska *obj) {
	EBML_ElementSetSizeLength((ebml_element *)obj->segment, 8);
	EBML_ElementRenderHead((ebml_element *)obj->segment, obj->output, FALSE, NULL);
}

static int matroska_write_zeros(Matroska *obj, size_t nbZeros) {
	uint8_t *data = (uint8_t *)ms_new0(uint8_t, nbZeros);
	size_t written=0;
	
	if(data == NULL) {
		return 0;
	}
	Stream_Write(obj->output, data, nbZeros, &written);
	ms_free(data);
	return written;
}

static inline void matroska_mark_segment_info_position(Matroska *obj) {
	obj->segmentInfoPosition = Stream_Seek(obj->output, 0, SEEK_CUR);
}

static int matroska_go_to_segment_info_mark(Matroska *obj) {
	if(obj->segmentInfoPosition == -1) {
		return -1;
	}
	Stream_Seek(obj->output, obj->segmentInfoPosition, SEEK_SET);
	return 0;
}

static inline void matroska_go_to_file_end(Matroska *obj) {
	Stream_Seek(obj->output, 0, SEEK_END);
}

static inline void matroska_go_to_segment_begin(Matroska *obj) {
	Stream_Seek(obj->output, EBML_ElementPositionData((ebml_element *)obj->segment), SEEK_SET);
}

static inline void matroska_go_to_last_cluster_end(Matroska *obj) {
	Stream_Seek(obj->output, EBML_ElementPositionEnd((ebml_element *)obj->cluster), SEEK_SET);
}

static inline void matroska_go_to_segment_info_begin(Matroska *obj) {
	Stream_Seek(obj->output, EBML_ElementPosition((ebml_element *)obj->info), SEEK_SET);
}

static matroska_block *_matroska_first_block(const ebml_master *cluster, ms_bool_t *endOfCluster) {
	ebml_element *elt;
	*endOfCluster = FALSE;
	for(elt = EBML_MasterChildren(cluster);
		elt != NULL && !EBML_ElementIsType(elt, &MATROSKA_ContextSimpleBlock) && !EBML_ElementIsType(elt, &MATROSKA_ContextBlockGroup);
		elt = EBML_MasterNext(elt));
	if(elt == NULL) {
		*endOfCluster = TRUE;
		return NULL;
	} else if(EBML_ElementIsType(elt, &MATROSKA_ContextSimpleBlock)) {
		return (matroska_block *)elt;
	} else if(EBML_ElementIsType(elt, &MATROSKA_ContextBlockGroup)) {
		return (matroska_block *)EBML_MasterFindChild(elt, &MATROSKA_ContextBlock);
	} else {
		return NULL;
	}
}

static matroska_block *_matroska_next_block(const matroska_block *block, ms_bool_t *endOfCluster) {
	ebml_element *elt;
	*endOfCluster = FALSE;
	if(EBML_ElementIsType((ebml_element *)block, &MATROSKA_ContextSimpleBlock)) {
		for(elt = EBML_MasterNext((ebml_element *)block);
			elt != NULL && !EBML_ElementIsType(elt, &MATROSKA_ContextSimpleBlock) && !EBML_ElementIsType(elt, &MATROSKA_ContextBlockGroup);
			elt = EBML_MasterNext(elt));
	} else {
		ebml_master *blockGroup = EBML_ElementParent((ebml_element *)block);
		for(elt = EBML_MasterNext((ebml_element *)blockGroup);
			elt != NULL && !EBML_ElementIsType(elt, &MATROSKA_ContextSimpleBlock) && !EBML_ElementIsType(elt, &MATROSKA_ContextBlockGroup);
			elt = EBML_MasterNext(elt));
	}
	if(elt == NULL) {
		*endOfCluster = TRUE;
		return NULL;
	} else if(EBML_ElementIsType(elt, &MATROSKA_ContextSimpleBlock)) {
		return (matroska_block *)elt;
	} else {
		return (matroska_block *)EBML_MasterFindChild((ebml_master *)elt, &MATROSKA_ContextBlock);
	}
}

static int matroska_block_go_first(Matroska *obj) {
	ms_bool_t endOfCluster;
	obj->currentCluster = obj->firstCluster;
	obj->currentBlock = _matroska_first_block(obj->firstCluster, &endOfCluster);
	if(obj->currentBlock != NULL || endOfCluster) {
		return 0;
	} else {
		return -1;
	}
}

static int matroska_block_go_next(Matroska *obj, ms_bool_t *eof) {
	ms_bool_t endOfCluster;
	*eof = FALSE;
	obj->currentBlock = _matroska_next_block(obj->currentBlock, &endOfCluster);
	if(!endOfCluster && obj->currentBlock == NULL) {
		return -1;
	} else if(!endOfCluster && obj->currentBlock != NULL) {
		return 0;
	} else {
		obj->currentCluster = (ebml_master *)EBML_MasterFindNextElt(obj->segment, (ebml_element *)obj->currentCluster, FALSE, FALSE);
		if(obj->currentCluster == NULL) {
			*eof = TRUE;
			return 0;
		} else {
			obj->currentBlock = _matroska_first_block(obj->currentCluster, &endOfCluster);
			if(endOfCluster) {
				*eof = TRUE;
				return 0;
			} else if(obj->currentBlock == NULL){
				return -2;
			} else {
				return 0;
			}
		}
	}
}

static inline timecode_t matroska_block_get_timestamp(const Matroska *obj) {
	return MATROSKA_BlockTimecode((matroska_block *)obj->currentBlock)/obj->timecodeScale;
}

static mblk_t *matroska_block_read_frame(Matroska *obj) {
	matroska_frame frame;
	MATROSKA_BlockReadData(obj->currentBlock, obj->output);
	MATROSKA_BlockGetFrame(obj->currentBlock, 0, &frame, TRUE);
	mblk_t *frameBuffer = allocb(frame.Size, 0);
	memcpy(frameBuffer->b_wptr, frame.Data, frame.Size);
	frameBuffer->b_wptr += frame.Size;
	mblk_set_timestamp_info(frameBuffer, frame.Timecode);
	MATROSKA_BlockReleaseData(obj->currentBlock, TRUE);
	return frameBuffer;
}

static inline int matroska_block_get_track_num(const Matroska *obj) {
	return MATROSKA_BlockTrackNum((matroska_block *)obj->currentBlock);
}

static int ebml_element_cmp_position(const void *a, const void *b) {
	return EBML_ElementPosition((ebml_element *)a) - EBML_ElementPosition((ebml_element *)b);
}

static void ebml_master_sort(ebml_master *master_elt) {
	MSList *elts = NULL;
	ebml_element *elt;
	for(elt = EBML_MasterChildren(master_elt); elt != NULL; elt = EBML_MasterNext(elt)) {
		elts = ms_list_insert_sorted(elts, elt, (MSCompareFunc)ebml_element_cmp_position);
	}
	EBML_MasterClear(master_elt);
	MSList *it;
	for(it = elts; it != NULL; it = ms_list_next(it)) {
		EBML_MasterAppend(master_elt, (ebml_element *)it->data);
	}
	ms_list_free(elts);
}

static int ebml_master_fill_blanks(stream *output, ebml_master *master) {
	MSList *voids = NULL;
	ebml_element *elt1, *elt2;
	for(elt1 = EBML_MasterChildren(master), elt2 = EBML_MasterNext(elt1); elt2 != NULL; elt1 = EBML_MasterNext(elt1), elt2 = EBML_MasterNext(elt2)) {
		filepos_t elt1_end_pos = EBML_ElementPositionEnd(elt1);
		filepos_t elt2_pos = EBML_ElementPosition(elt2);
		int interval = elt2_pos - elt1_end_pos;
		if(interval < 0) {
			return -1; // Elements are neither contigus or distinct.
		} else if(interval == 0) {
			// Nothing to do. Elements are contigus.
		} else if(interval > 0 && interval < 2) {
			return -2; // Not enough space to write a void element.
		} else {
			ebml_element *voidElt = EBML_ElementCreate(master, &EBML_ContextEbmlVoid, TRUE, NULL);
			EBML_VoidSetFullSize(voidElt, interval);
			Stream_Seek(output, elt1_end_pos, SEEK_SET);
			EBML_ElementRender(voidElt, output, FALSE, FALSE, FALSE, NULL);
			voids = ms_list_append(voids, voidElt);
		}
	}

	MSList *it;
	for(it = voids; it != NULL; it = ms_list_next(it)) {
		EBML_MasterAppend(master, (ebml_element *)it->data);
	}
	ms_list_free(voids);
	return 0;
}

static void ebml_master_delete_empty_elements(ebml_master *master) {
	ebml_element *child;
	for(child = EBML_MasterChildren(master); child != NULL; child = EBML_MasterNext(child)) {
		if(EBML_ElementDataSize(child, WRITE_DEFAULT_ELEMENT) <= 0) {
			EBML_MasterRemove(master, child);
			NodeDelete((node *)child);
		}
	}
}

static int matroska_close_segment(Matroska *obj) {
	filepos_t initialPos = Stream_Seek(obj->output, 0, SEEK_CUR);
	EBML_ElementUpdateSize(obj->segment, WRITE_DEFAULT_ELEMENT, FALSE);
	ebml_master_delete_empty_elements(obj->segment);
	ebml_master_sort(obj->segment);
	if(ebml_master_fill_blanks(obj->output, obj->segment) < 0) {
		return -1;
	}
	updateElementHeader((ebml_element *)obj->segment, obj->output);
	Stream_Seek(obj->output, initialPos, SEEK_SET);
	return 0;
}

static ebml_master *matroska_find_track_entry(const Matroska *obj, int trackNum) {
	ebml_element *trackEntry;
	for(trackEntry = EBML_MasterChildren(obj->tracks);
		trackEntry != NULL && EBML_IntegerValue((ebml_integer *)EBML_MasterFindChild((ebml_master *)trackEntry, &MATROSKA_ContextTrackNumber)) != trackNum;
		trackEntry = EBML_MasterNext(trackEntry));
	return (ebml_master *)trackEntry;
}

static int matroska_get_codec_private(const Matroska *obj, int trackNum, const uint8_t **data, size_t *length) {
	ebml_master *trackEntry = matroska_find_track_entry(obj, trackNum);
	if(trackEntry == NULL)
		return -1;
	ebml_binary *codecPrivate = (ebml_binary *)EBML_MasterFindChild(trackEntry, &MATROSKA_ContextCodecPrivate);
	if(codecPrivate == NULL)
		return -2;

	*length = EBML_ElementDataSize((ebml_element *)codecPrivate, FALSE);
	*data =  EBML_BinaryGetData(codecPrivate);
	if(*data == NULL)
		return -3;
	else
		return 0;
}

static void matroska_start_cluster(Matroska *obj, timecode_t clusterTimecode) {
	obj->cluster = (ebml_master *)EBML_MasterAddElt(obj->segment, &MATROSKA_ContextCluster, TRUE);
	if(obj->nbClusters == 0) {
		obj->firstCluster = obj->cluster;
	}
	EBML_ElementSetSizeLength((ebml_element *)obj->cluster, 8);
	EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild(obj->cluster, &MATROSKA_ContextTimecode), clusterTimecode);
	EBML_ElementRender((ebml_element *)obj->cluster, obj->output, WRITE_DEFAULT_ELEMENT, FALSE, FALSE, NULL);
	obj->nbClusters++;
}

static void matroska_close_cluster(Matroska *obj) {
	ebml_element *block;
	if(obj->cluster == NULL) {
		return;
	} else {
		block = EBML_MasterFindChild(obj->cluster, &MATROSKA_ContextSimpleBlock);
	}
	if(block == NULL) {
		ebml_element *voidElt = EBML_ElementCreate(obj->p, &EBML_ContextEbmlVoid, FALSE, NULL);
		EBML_MasterAppend(obj->segment, voidElt);
		EBML_VoidSetFullSize(voidElt, EBML_ElementFullSize((ebml_element *)obj->cluster, WRITE_DEFAULT_ELEMENT));
		Stream_Seek((ebml_master*)obj->output, EBML_ElementPosition((ebml_element *)obj->cluster), SEEK_SET);
		EBML_ElementRender(voidElt, obj->output, FALSE, FALSE, FALSE, NULL);
		EBML_MasterRemove(obj->segment, (ebml_element *)obj->cluster);
		NodeDelete((node *)obj->cluster);
		obj->cluster = NULL;
	} else {
		updateElementHeader((ebml_element *)obj->cluster, obj->output);
	}
}

static inline int matroska_clusters_count(const Matroska *obj) {
	return obj->nbClusters;
}

static inline timecode_t matroska_current_cluster_timecode(const Matroska *obj) {
	return EBML_IntegerValue((ebml_integer *)EBML_MasterFindChild(obj->cluster, &MATROSKA_ContextTimecode));
}

static ebml_master *matroska_find_track(const Matroska *obj, int trackNum) {
	ebml_element *elt;
	for(elt = EBML_MasterChildren(obj->tracks);
		elt != NULL && EBML_IntegerValue((ebml_integer *)EBML_MasterFindChild(elt, &MATROSKA_ContextTrackNumber)) != trackNum;
		elt = EBML_MasterNext(elt));
	return (ebml_master *)elt;
}

static int matroska_add_track(Matroska *obj, int trackNum, const char codecID[]) {
	ebml_master *track = matroska_find_track(obj, trackNum);
	if(track != NULL) {
		return -1;
	} else {
		track = (ebml_master *)EBML_MasterAddElt(obj->tracks, &MATROSKA_ContextTrackEntry, FALSE);
		if(track == NULL) {
			return -2;
		}
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild(track, &MATROSKA_ContextTrackNumber), trackNum);
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild(track, &MATROSKA_ContextTrackUID), trackNum);
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild(track, &MATROSKA_ContextFlagEnabled), 1);
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild(track, &MATROSKA_ContextFlagDefault), 1);
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild(track, &MATROSKA_ContextFlagForced), 0);
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild(track, &MATROSKA_ContextFlagLacing), 0);
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild(track, &MATROSKA_ContextMinCache), 1);
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild(track, &MATROSKA_ContextMaxBlockAdditionID), 0);
		EBML_StringSetValue((ebml_string *)EBML_MasterGetChild(track, &MATROSKA_ContextCodecID), codecID);
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild(track, &MATROSKA_ContextCodecDecodeAll), 0);
		return 0;
	}
}

static int matroska_del_track(Matroska *obj, int trackNum) {
	ebml_master *track = matroska_find_track(obj, trackNum);
	if(track == NULL) {
		return -1;
	} else {
		if(EBML_MasterRemove(obj->tracks, (ebml_element *)track) != ERR_NONE) {
			return -2;
		} else {
			NodeDelete((node *)track);
			return 0;
		}
	}
}

static int matroska_get_default_track_num(const Matroska *obj, int trackType) {
	ebml_element *track;
	for(track = EBML_MasterChildren(obj->tracks); track != NULL; track = EBML_MasterNext(track)) {
		ebml_integer *trackTypeElt = (ebml_integer *)EBML_MasterFindChild(track, &MATROSKA_ContextTrackType);
		ebml_integer *flagDefault = (ebml_integer *)EBML_MasterFindChild(track, &MATROSKA_ContextFlagDefault);
		if(trackTypeElt != NULL && flagDefault != NULL && EBML_IntegerValue(trackTypeElt) == trackType && EBML_IntegerValue(flagDefault) == 1) {
			break;
		}
	}
	if(track == NULL) {
		return -1;
	} else {
		ebml_integer *trackNum = (ebml_integer *)EBML_MasterFindChild(track, &MATROSKA_ContextTrackNumber);
		if(trackNum == NULL) {
			return -2;
		} else {
			return EBML_IntegerValue(trackNum);
		}
	}
}

static int matroska_get_first_track_num_from_type(const Matroska *obj, int trackType) {
	ebml_element *track;
	for(track = EBML_MasterChildren(obj->tracks); track != NULL; track = EBML_MasterNext(track)) {
		if(EBML_IntegerValue((ebml_integer *)EBML_MasterFindChild((ebml_master *)track, &MATROSKA_ContextTrackType)) == trackType) {
			break;
		}
	}
	if(track == NULL) {
		return 0;
	} else {
		return EBML_IntegerValue((ebml_integer *)EBML_MasterFindChild((ebml_master *)track, &MATROSKA_ContextTrackNumber));
	}
}

static int matroska_track_set_codec_private(Matroska *obj, int trackNum, const uint8_t *data, size_t dataSize) {
	ebml_master *track = matroska_find_track(obj, trackNum);
	if(track == NULL) {
		return -1;
	} else {
		ebml_binary *codecPrivate = EBML_MasterGetChild(track, &MATROSKA_ContextCodecPrivate);
		if(EBML_BinarySetData(codecPrivate, data, dataSize) != ERR_NONE) {
			return -2;
		} else {
			return 0;
		}
	}
}

static int matroska_track_set_info(Matroska *obj, int trackNum, const MSFmtDescriptor *fmt) {
	ebml_master *track = matroska_find_track(obj, trackNum);
	if(track == NULL) {
		return -1;
	} else {
		switch(fmt->type) {
		case MSVideo:
			EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild(track, &MATROSKA_ContextTrackType), TRACK_TYPE_VIDEO);
			ebml_master *videoInfo = (ebml_master *)EBML_MasterGetChild(track, &MATROSKA_ContextVideo);
			EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild(videoInfo, &MATROSKA_ContextFlagInterlaced), 0);
			EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild(videoInfo, &MATROSKA_ContextPixelWidth), fmt->vsize.width);
			EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild(videoInfo, &MATROSKA_ContextPixelHeight), fmt->vsize.height);
			break;

		case MSAudio:
			EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild(track, &MATROSKA_ContextTrackType), TRACK_TYPE_AUDIO);
			ebml_master *audioInfo = (ebml_master *)EBML_MasterGetChild(track, &MATROSKA_ContextAudio);
			EBML_FloatSetValue((ebml_float *)EBML_MasterGetChild(audioInfo, &MATROSKA_ContextSamplingFrequency), fmt->rate);
			EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild(audioInfo, &MATROSKA_ContextChannels), fmt->nchannels);
			break;
		}
		return 0;
	}
}

static int matroska_track_get_info(const Matroska *obj, int trackNum, const MSFmtDescriptor **fmt, const uint8_t **codecPrivateData, size_t *codecPrivateSize) {
	ebml_master *track = matroska_find_track(obj, trackNum);
	*fmt = NULL;
	if(track == NULL) {
		return -1;
	} else {
		if(!EBML_MasterCheckMandatory(track, FALSE)) {
			return -2;
		} else {
			char codecId[50];
			const char *rfcName;
			int trackType;
			ebml_element *elt;
			ebml_binary *codecPrivate;
			EBML_StringGet((ebml_string *)EBML_MasterFindChild((ebml_master *)track, &MATROSKA_ContextCodecID), codecId, 50);
			rfcName = codec_id_to_rfc_name(codecId);
			if(rfcName == NULL) {
				return -3;
			}
			if((codecPrivate = (ebml_binary *)EBML_MasterFindChild(track, &MATROSKA_ContextCodecPrivate)) == NULL) {
				*codecPrivateData = NULL;
			} else {
				*codecPrivateData = EBML_BinaryGetData(codecPrivate);
				*codecPrivateSize = EBML_ElementDataSize((ebml_element *)codecPrivate, FALSE);
			}
			trackType = EBML_IntegerValue((ebml_integer *)EBML_MasterFindChild((ebml_master *)track, &MATROSKA_ContextTrackType));
			switch(trackType) {
			case TRACK_TYPE_VIDEO:
				elt = EBML_MasterFindChild((ebml_master *)track, &MATROSKA_ContextVideo);
				if(elt == NULL || !EBML_MasterCheckMandatory((ebml_master *)elt, FALSE)) {
					return -4;
				} else {
					MSVideoSize vsize;
					vsize.width = EBML_IntegerValue((ebml_integer *)EBML_MasterFindChild((ebml_master *)elt, &MATROSKA_ContextPixelWidth));
					vsize.height = EBML_IntegerValue((ebml_integer *)EBML_MasterFindChild((ebml_master *)elt, &MATROSKA_ContextPixelHeight));
					*fmt = ms_factory_get_video_format(ms_factory_get_fallback(), rfcName, &vsize, NULL);
				}
				break;

			case TRACK_TYPE_AUDIO:
				elt = EBML_MasterFindChild((ebml_master *)track, &MATROSKA_ContextAudio);
				if(elt == NULL || !EBML_MasterCheckMandatory((ebml_master *)elt, FALSE)) {
					return -4;
				} else {
					int rate, nbChannels;
					rate = EBML_IntegerValue((ebml_integer *)EBML_MasterFindChild((ebml_master *)elt, &MATROSKA_ContextSamplingFrequency));
					nbChannels = EBML_IntegerValue((ebml_integer *)EBML_MasterFindChild((ebml_master *)elt, &MATROSKA_ContextChannels));
					*fmt = ms_factory_get_audio_format(ms_factory_get_fallback(), rfcName, rate, nbChannels, NULL);
				}
				break;

			default:
				return -5;
			}
			return 0;
		}
	}
}

static ms_bool_t matroska_track_check_block_presence(Matroska *obj, int trackNum) {
	ebml_element *elt1;
	for(elt1 = EBML_MasterChildren(obj->segment); elt1 != NULL; elt1 = EBML_MasterNext(elt1)) {
		if(EBML_ElementIsType(elt1, &MATROSKA_ContextCluster)) {
			ebml_element *cluster = elt1, *elt2;
			for(elt2 = EBML_MasterChildren(cluster); elt2 != NULL; elt2 = EBML_MasterNext(elt2)) {
				if(EBML_ElementIsType(elt2, &MATROSKA_ContextSimpleBlock)) {
					matroska_block *block = (matroska_block *)elt2;
					if(MATROSKA_BlockTrackNum(block) == trackNum) {
						break;
					}
				}
			}
			if(elt2 != NULL) {
				break;
			}
		}
	}
	if(elt1 == NULL) {
		return FALSE;
	} else {
		return TRUE;
	}
}

static int matroska_add_cue(Matroska *obj, matroska_block *block) {
	matroska_cuepoint *cue = (matroska_cuepoint *)EBML_MasterAddElt(obj->cues, &MATROSKA_ContextCuePoint, TRUE);
	if(cue == NULL) {
		return -1;
	} else {
		MATROSKA_LinkCuePointBlock(cue, block);
		MATROSKA_LinkCueSegmentInfo(cue, obj->info);
		MATROSKA_CuePointUpdate(cue, (ebml_element *)obj->segment);
		return 0;
	}
}

static inline void matroska_write_segment_info(Matroska *obj) {
	EBML_ElementRender((ebml_element *)obj->info, obj->output, WRITE_DEFAULT_ELEMENT, FALSE, FALSE, NULL);
	MATROSKA_MetaSeekUpdate(obj->infoMeta);
}

static inline void matroska_write_tracks(Matroska *obj) {
	EBML_ElementRender((ebml_element *)obj->tracks, obj->output, WRITE_DEFAULT_ELEMENT, FALSE, FALSE, NULL);
	MATROSKA_MetaSeekUpdate(obj->tracksMeta);
}

static int matroska_write_cues(Matroska *obj) {
	if(EBML_MasterChildren(obj->cues) != NULL) {
		EBML_ElementRender((ebml_element *)obj->cues, obj->output, WRITE_DEFAULT_ELEMENT, FALSE, FALSE, NULL);
		MATROSKA_MetaSeekUpdate(obj->cuesMeta);
		return 0;
	} else {
		EBML_MasterRemove(obj->segment, (ebml_element *)obj->cues);
		EBML_MasterRemove(obj->metaSeek, (ebml_element *)obj->cuesMeta);
		Node_Release((node *)obj->cues);
		Node_Release((node *)obj->cuesMeta);
		return -1;
	}
}

static inline void matroska_write_metaSeek(Matroska *obj) {
	EBML_ElementRender((ebml_element *)obj->metaSeek, obj->output, WRITE_DEFAULT_ELEMENT, FALSE, FALSE, NULL);
}

static inline timecode_t matroska_get_current_cluster_timestamp(const Matroska *obj) {
	return (timecode_t)EBML_IntegerValue((ebml_integer *)EBML_MasterGetChild(obj->cluster, &MATROSKA_ContextTimecode));
}

static matroska_block *matroska_write_block(Matroska *obj, const matroska_frame *m_frame, int trackNum, ms_bool_t isKeyFrame) {
	if(obj->timecodeScale == -1) {
		return NULL;
	} else {
		matroska_block *block = (matroska_block *)EBML_MasterAddElt(obj->cluster, &MATROSKA_ContextSimpleBlock, FALSE);
		block->TrackNumber = trackNum;
		MATROSKA_LinkBlockWithReadTracks(block, obj->tracks, TRUE);
		MATROSKA_LinkBlockWriteSegmentInfo(block, obj->info);
		MATROSKA_BlockSetKeyframe(block, isKeyFrame);
		MATROSKA_BlockSetDiscardable(block, FALSE);
		timecode_t clusterTimecode = EBML_IntegerValue((ebml_integer *)EBML_MasterGetChild(obj->cluster, &MATROSKA_ContextTimecode));
		MATROSKA_BlockAppendFrame(block, m_frame, clusterTimecode * obj->timecodeScale);
		EBML_ElementRender((ebml_element *)block, obj->output, WRITE_DEFAULT_ELEMENT, FALSE, FALSE, NULL);
		MATROSKA_BlockReleaseData(block, TRUE);
		return block;
	}
}

/*********************************************************************************************
 * Muxer                                                                                     *
 *********************************************************************************************/
typedef struct {
	int nqueues;
	MSQueue *queues;
} Muxer;

static void muxer_init(Muxer *obj, int ninputs) {
	int i;
	obj->nqueues = ninputs;
	obj->queues = (MSQueue *)ms_new0(MSQueue, ninputs);
	for(i=0; i < ninputs; i++) {
		ms_queue_init(&obj->queues[i]);
	}
}

static void muxer_empty_internal_queues(Muxer *obj) {
	int i;
	for(i=0; i< obj->nqueues; i++) {
		ms_queue_flush(&obj->queues[i]);
	}
}

static void muxer_uninit(Muxer *obj) {
	muxer_empty_internal_queues(obj);
	ms_free(obj->queues);
}

static inline void muxer_put_buffer(Muxer *obj, mblk_t *buffer, int pin) {
	ms_queue_put(&obj->queues[pin], buffer);
}

static mblk_t *muxer_get_buffer(Muxer *obj, int *pin) {
	int i;
	MSQueue *minQueue = NULL;

	for(i=0; i < obj->nqueues; i++) {
		if(!ms_queue_empty(&obj->queues[i])) {
			if(minQueue == NULL) {
				minQueue = &obj->queues[i];
			} else {
				uint32_t t1 = mblk_get_timestamp_info(qfirst(&minQueue->q));
				uint32_t t2 = mblk_get_timestamp_info(qfirst(&(&obj->queues[i])->q));
				if(t2 < t1) {
					minQueue = &obj->queues[i];
				}
			}
		}
	}
	if(minQueue == NULL) {
		return NULL;
	} else {
		*pin = (minQueue - obj->queues);
		return ms_queue_get(minQueue);
	}
}

/*********************************************************************************************
 * Timestamp corrector                                                                       *
 *********************************************************************************************/
typedef struct {
	timecode_t globalOrigin;
	timecode_t globalOffset;
	timecode_t *offsetList;
	ms_bool_t globalOffsetIsSet;
	ms_bool_t *offsetIsSet;
	int nPins;
	const MSTicker *ticker;
} TimeCorrector;

static void time_corrector_init(TimeCorrector *obj, int nPins) {
	obj->globalOrigin = 0;
	obj->nPins = nPins;
	obj->offsetList = (timecode_t *)ms_new0(timecode_t, obj->nPins);
	obj->offsetIsSet = (ms_bool_t *)ms_new0(ms_bool_t, obj->nPins);
	obj->globalOffsetIsSet = FALSE;
	obj->ticker = NULL;
}

static void time_corrector_uninit(TimeCorrector *obj) {
	ms_free(obj->offsetList);
	ms_free(obj->offsetIsSet);
}

static inline void time_corrector_set_origin(TimeCorrector *obj, timecode_t origin) {
	obj->globalOrigin = origin;
}

static inline void time_corrector_set_ticker(TimeCorrector *obj, const MSTicker *ticker) {
	obj->ticker = ticker;
}

static void time_corrector_reset(TimeCorrector *obj) {
	int i;
	obj->globalOffsetIsSet = FALSE;
	for(i=0; i < obj->nPins; i++) {
		obj->offsetIsSet[i] = FALSE;
	}
}

static void time_corrector_proceed(TimeCorrector *obj, mblk_t *buffer, int pin) {
	if(!obj->globalOffsetIsSet) {
		obj->globalOffset = obj->globalOrigin - obj->ticker->time;
		obj->globalOffsetIsSet = TRUE;
	}
	if(!obj->offsetIsSet[pin]) {
		uint64_t origin = obj->ticker->time + obj->globalOffset;
		obj->offsetList[pin] = origin - mblk_get_timestamp_info(buffer);
		obj->offsetIsSet[pin] = TRUE;
	}
	mblk_set_timestamp_info(buffer, mblk_get_timestamp_info(buffer) + obj->offsetList[pin]);
}


/*********************************************************************************************
 * MKV Recorder Filter                                                                       *
 *********************************************************************************************/
#define CLUSTER_MAX_DURATION 5000

typedef struct {
	Matroska file;
	timecode_t duration;
	MatroskaOpenMode openMode;
	MSRecorderState state;
	Muxer muxer;
	TimeCorrector timeCorrector;
	ms_bool_t needKeyFrame;
	const MSFmtDescriptor **inputDescsList;
	Module **modulesList;
} MKVRecorder;

static void recorder_init(MSFilter *f) {
	MKVRecorder *obj = (MKVRecorder *)ms_new0(MKVRecorder, 1);

	ms_message("MKVRecorder: initialisation");
	matroska_init(&obj->file);

	obj->state = MSRecorderClosed;
	obj->needKeyFrame = TRUE;

	muxer_init(&obj->muxer, f->desc->ninputs);

	obj->inputDescsList = (const MSFmtDescriptor **)ms_new0(const MSFmtDescriptor *, f->desc->ninputs);
	obj->modulesList = (Module **)ms_new0(Module *, f->desc->ninputs);

	time_corrector_init(&obj->timeCorrector, f->desc->ninputs);

	f->data=obj;
}

static void recorder_uninit(MSFilter *f){
	MKVRecorder *obj = (MKVRecorder *)f->data;
	int i;

	muxer_uninit(&obj->muxer);
	if(obj->state != MSRecorderClosed) {
		matroska_close_file(&obj->file);
	}
	matroska_uninit(&obj->file);
	for(i=0; i < f->desc->ninputs; i++) {
		if(obj->modulesList[i] != NULL) {
			module_free(obj->modulesList[i]);
		}
		if(f->inputs[i] != NULL) {
			ms_queue_flush(f->inputs[i]);
		}
	}
	time_corrector_uninit(&obj->timeCorrector);
	ms_free(obj->modulesList);
	ms_free(obj->inputDescsList);
	ms_free(obj);
	ms_message("MKVRecorder: destroyed");
}

static void recorder_preprocess(MSFilter *f) {
	MKVRecorder *obj = (MKVRecorder *)f->data;
	ms_filter_lock(f);
	time_corrector_set_ticker(&obj->timeCorrector, f->ticker);
	ms_filter_unlock(f);
}

static void recorder_postprocess(MSFilter *f) {
	MKVRecorder *obj = (MKVRecorder *)f->data;
	ms_filter_lock(f);
	time_corrector_set_ticker(&obj->timeCorrector, NULL);
	ms_filter_unlock(f);
}

static matroska_block *write_frame(MKVRecorder *obj, mblk_t *buffer, int pin) {
	ms_bool_t isKeyFrame;
	mblk_t *frame = module_process(obj->modulesList[pin], buffer, &isKeyFrame);

	matroska_frame m_frame;
	m_frame.Timecode = ((int64_t)mblk_get_timestamp_info(frame))*1000000LL;
	m_frame.Size = msgdsize(frame);
	m_frame.Data = frame->b_rptr;

	if(matroska_clusters_count(&obj->file) == 0) {
		matroska_start_cluster(&obj->file, mblk_get_timestamp_info(frame));
	} else {
		if((obj->inputDescsList[pin]->type == MSVideo && isKeyFrame) || (obj->duration - matroska_current_cluster_timecode(&obj->file) >= CLUSTER_MAX_DURATION)) {
			matroska_close_cluster(&obj->file);
			matroska_start_cluster(&obj->file, mblk_get_timestamp_info(frame));
		}
	}

	matroska_block *block = matroska_write_block(&obj->file, &m_frame, pin + 1, isKeyFrame);
	freemsg(frame);
	return block;
}

static void changeClockRate(mblk_t *buffer, uint32_t oldClockRate, uint32_t newClockRate) {
	mblk_t *curBuff;
	for(curBuff = buffer; curBuff != NULL; curBuff = curBuff->b_cont) {
		mblk_set_timestamp_info(curBuff, mblk_get_timestamp_info(curBuff) * newClockRate / oldClockRate);
	}
}

static int recorder_open_file(MSFilter *f, void *arg) {
	MKVRecorder *obj = (MKVRecorder *)f->data;
	const char *filename = (const char *)arg;
	int err = 0, i;

	ms_filter_lock(f);
	if(obj->state == MSRecorderClosed) {
		if (access(filename, R_OK | W_OK) == 0) {
			obj->openMode = MKV_OPEN_APPEND;
		} else {
			obj->openMode = MKV_OPEN_CREATE;
		}

		ms_message("MKVRecoreder: opening file %s in %s mode", filename, obj->openMode == MKV_OPEN_APPEND ? "append" : "create");
		if(matroska_open_file(&obj->file, filename, obj->openMode) != 0) {
			err = -1;
			ms_error("MKVRecorder: fail to open %s", filename);
		} else {
			if(obj->openMode == MKV_OPEN_CREATE) {
				matroska_set_doctype_version(&obj->file, MKV_DOCTYPE_VERSION, MKV_DOCTYPE_READ_VERSION);
				matroska_write_ebml_header(&obj->file);
				matroska_start_segment(&obj->file);
				matroska_write_zeros(&obj->file, 1024);
				matroska_mark_segment_info_position(&obj->file);
				matroska_write_zeros(&obj->file, 1024);

				for(i=0; i < f->desc->ninputs; i++) {
					if(obj->inputDescsList[i] != NULL) {
						obj->modulesList[i] = module_new(obj->inputDescsList[i]->encoding);
						module_set(obj->modulesList[i], obj->inputDescsList[i]);
						matroska_add_track(&obj->file, i+1, module_get_codec_id(obj->modulesList[i]));
					}
				}
				obj->duration = 0;
			} else {
				for(i=0; i < f->desc->ninputs; i++) {
					if(obj->inputDescsList[i] != NULL) {
						const uint8_t *data;
						size_t length;
						obj->modulesList[i] = module_new(obj->inputDescsList[i]->encoding);
						module_set(obj->modulesList[i], obj->inputDescsList[i]);
						if(matroska_get_codec_private(&obj->file, i+1, &data, &length) == 0) {
							module_load_private_data(obj->modulesList[i], data, length);
						}
					}
				}
				obj->duration = matroska_get_duration(&obj->file);
			}
			time_corrector_set_origin(&obj->timeCorrector, obj->duration);
			obj->state = MSRecorderPaused;
			ms_message("%s successfully opened", filename);
		}
	}
	ms_filter_unlock(f);
	return err;
}

static int recorder_start(MSFilter *f, void *arg)
{
	MKVRecorder *obj = (MKVRecorder *)f->data;
	int err = 0;

	ms_filter_lock(f);
	if(obj->state == MSRecorderClosed) {
		ms_error("MKVRecorder: fail to start recording. The file has not been opened");
		err = -1;
	} else {
		obj->state = MSRecorderRunning;
		obj->needKeyFrame = TRUE;
		ms_message("MKVRecorder: recording successfully started");
	}
	ms_filter_unlock(f);
	return err;
}

static int recorder_stop(MSFilter *f, void *arg) {
	MKVRecorder *obj = (MKVRecorder *)f->data;
	int err=-1;

	ms_filter_lock(f);
	switch(obj->state) {
	case MSRecorderClosed:
		err = -1;
		ms_error("MKVRecorder: fail to stop recording. The file has not been opened");
		break;
	case MSRecorderPaused:
		err=0;
		ms_warning("MKVRecorder: recording has already been stopped");
		break;
	case MSRecorderRunning:
		obj->state = MSRecorderPaused;
		muxer_empty_internal_queues(&obj->muxer);
		err = 0;
		ms_message("MKVRecorder: recording successfully stopped");
		break;
	}
	ms_filter_unlock(f);
	return err;
}

static void recorder_process(MSFilter *f) {
	MKVRecorder *obj = f->data;
	int i;

	ms_filter_lock(f);
	if(obj->state != MSRecorderRunning) {
		for(i=0; i < f->desc->ninputs; i++) {
			if(f->inputs[i] != NULL) {
				ms_queue_flush(f->inputs[i]);
			}
		}
	} else {
		int pin;
		mblk_t *buffer;
		for(i=0; i < f->desc->ninputs; i++) {
			if(f->inputs[i] != NULL && obj->inputDescsList[i] == NULL) {
				ms_queue_flush(f->inputs[i]);
			}
			else if(f->inputs[i] != NULL && obj->inputDescsList[i] != NULL) {
				MSQueue frames, frames_ms;
				ms_queue_init(&frames);
				ms_queue_init(&frames_ms);

				module_preprocess(obj->modulesList[i], f->inputs[i], &frames);
				while((buffer = ms_queue_get(&frames)) != NULL) {
					changeClockRate(buffer, obj->inputDescsList[i]->rate, 1000);
					ms_queue_put(&frames_ms, buffer);
				}

				if(obj->inputDescsList[i]->type == MSVideo && obj->needKeyFrame) {
					while((buffer = ms_queue_get(&frames_ms)) != NULL) {
						if(module_is_key_frame(obj->modulesList[i], buffer)) {
							break;
						} else {
							freemsg(buffer);
						}
					}
					if(buffer != NULL) {
						do {
							time_corrector_proceed(&obj->timeCorrector, buffer, i);
							muxer_put_buffer(&obj->muxer, buffer, i);
						} while((buffer = ms_queue_get(&frames_ms)) != NULL);
						obj->needKeyFrame = FALSE;
					}
				} else {
					while((buffer = ms_queue_get(&frames_ms)) != NULL) {
						time_corrector_proceed(&obj->timeCorrector, buffer, i);
						muxer_put_buffer(&obj->muxer, buffer, i);
					}
				}
			}
		}
		while((buffer = muxer_get_buffer(&obj->muxer, &pin)) != NULL) {
			matroska_block *block;
			timecode_t bufferTimecode;

			bufferTimecode = mblk_get_timestamp_info(buffer);

			block = write_frame(obj, buffer, pin);

			if(obj->inputDescsList[pin]->type == MSVideo) {
				matroska_add_cue(&obj->file, block);
			}
			if(bufferTimecode > obj->duration) {
				obj->duration = bufferTimecode;
			}
		}
	}
	ms_filter_unlock(f);
}

static int recorder_close(MSFilter *f, void *arg) {
	MKVRecorder *obj = (MKVRecorder *)f->data;
	int i;

	ms_filter_lock(f);
	ms_message("MKVRecorder: closing file");
	if(obj->state != MSRecorderClosed) {
		for(i=0; i < f->desc->ninputs; i++) {
			if(f->inputs[i] != NULL) {
				ms_queue_flush(f->inputs[i]);
			}
			if(obj->inputDescsList[i] != NULL) {
				if(matroska_track_check_block_presence(&obj->file, i+1)) {
					uint8_t *codecPrivateData;
					size_t codecPrivateDataSize;
					module_get_private_data(obj->modulesList[i], &codecPrivateData, &codecPrivateDataSize);
					matroska_track_set_info(&obj->file, i+1, obj->inputDescsList[i]);
					if(codecPrivateDataSize > 0) {
						matroska_track_set_codec_private(&obj->file, i + 1, codecPrivateData, codecPrivateDataSize);
					}
					ms_free(codecPrivateData);
				} else {
					matroska_del_track(&obj->file, i+1);
				}
				module_free(obj->modulesList[i]);
				obj->modulesList[i] = NULL;
			}
		}
		matroska_close_cluster(&obj->file);
		if(matroska_write_cues(&obj->file) != 0) {
			ms_warning("MKVRecorder: no cues written");
		}

		if(obj->openMode == MKV_OPEN_CREATE) {
			matroska_go_to_segment_info_mark(&obj->file);
		} else {
			matroska_go_to_segment_info_begin(&obj->file);
		}
		obj->duration++;
		matroska_set_segment_info(&obj->file, "libmediastreamer2", "libmediastreamer2", obj->duration);
		matroska_write_segment_info(&obj->file);
		matroska_write_tracks(&obj->file);
		matroska_go_to_segment_begin(&obj->file);
		matroska_write_metaSeek(&obj->file);
		matroska_go_to_file_end(&obj->file);
		matroska_close_segment(&obj->file);
		matroska_close_file(&obj->file);
		time_corrector_reset(&obj->timeCorrector);
		obj->state = MSRecorderClosed;
		ms_message("MKVRecorder: the file has been successfully closed");
	} else {
		ms_warning("MKVRecorder: no file has been opened");
	}
	ms_filter_unlock(f);

	return 0;
}

static int recorder_set_input_fmt(MSFilter *f, void *arg) {
	MKVRecorder *data=(MKVRecorder *)f->data;
	const MSPinFormat *pinFmt = (const MSPinFormat *)arg;
	int err = 0;

	ms_filter_lock(f);
	if(pinFmt->pin < 0 || pinFmt->pin >= f->desc->ninputs) {
		ms_error("MKVRecorder: could not set pin #%d. Invalid pin number", pinFmt->pin);
		err = -1;
	} else {
		if(data->state != MSRecorderClosed) {
			if(pinFmt->fmt == NULL) {
				ms_error("MKVRecorder: could not disable pin #%d. The file is opened", pinFmt->pin);
				err = -3;
			} else  if(data->inputDescsList[pinFmt->pin] == NULL) {
				ms_error("MKVRecorder: could not set pin #%d video size. That pin is not enabled", pinFmt->pin);
				err = -4;
			} else if(pinFmt->fmt->type != MSVideo ||
					  strcmp(pinFmt->fmt->encoding, data->inputDescsList[pinFmt->pin]->encoding) != 0 ||
					  pinFmt->fmt->rate != data->inputDescsList[pinFmt->pin]->rate) {
				ms_error("MKVRecorder: could not set pin #%d video size. The specified format is not compatible with the current format. current={%s}, new={%s}",
						 pinFmt->pin,
						 ms_fmt_descriptor_to_string(data->inputDescsList[pinFmt->pin]),
						 ms_fmt_descriptor_to_string(pinFmt->fmt));
				err = -5;
			} else {
				data->inputDescsList[pinFmt->pin] = pinFmt->fmt;
				ms_message("MKVRecorder: pin #%d video size set on %dx%d", pinFmt->pin, pinFmt->fmt->vsize.width, pinFmt->fmt->vsize.height);
				err = 0;
			}
		} else {
			if(pinFmt->fmt == NULL) {
				data->inputDescsList[pinFmt->pin] = pinFmt->fmt;
				ms_message("MKVRecorder: pin #%d set as disabled", pinFmt->pin);
				err = 0;
			} else if(find_module_id_from_rfc_name(pinFmt->fmt->encoding) == NONE_ID) {
				ms_error("MKVRecorder: could not set pin #%d. %s is not supported", pinFmt->pin, pinFmt->fmt->encoding);
				err = -2;
			} else {
				data->inputDescsList[pinFmt->pin] = pinFmt->fmt;
				ms_message("MKVRecorder: set pin #%d format. %s", pinFmt->pin, ms_fmt_descriptor_to_string(pinFmt->fmt));
				err = 0;
			}
		}
	}
	ms_filter_unlock(f);
	return err;
}

static int recorder_get_state(MSFilter *f, void *data){
	MKVRecorder *priv=(MKVRecorder *)f->data;
	*(MSRecorderState*)data=priv->state;
	return 0;
}

static MSFilterMethod recorder_methods[]= {
	{	MS_RECORDER_OPEN            ,	recorder_open_file         },
	{	MS_RECORDER_CLOSE           ,	recorder_close             },
	{	MS_RECORDER_START           ,	recorder_start             },
	{	MS_RECORDER_PAUSE           ,	recorder_stop              },
	{	MS_FILTER_SET_INPUT_FMT     ,   recorder_set_input_fmt     },
	{	MS_RECORDER_GET_STATE	    ,	recorder_get_state         },
	{	0                           ,   NULL                       }
};

#ifdef _MSC_VER
MSFilterDesc ms_mkv_recorder_desc= {
	MS_MKV_RECORDER_ID,
	"MSMKVRecorder",
	"MKV file recorder",
	MS_FILTER_OTHER,
	2,
	0,
	recorder_init,
	recorder_preprocess,
	recorder_process,
	recorder_postprocess,
	recorder_uninit,
	recorder_methods
};
#else
MSFilterDesc ms_mkv_recorder_desc={
	.id=MS_MKV_RECORDER_ID,
	.name="MSMKVRecorder",
	.text="MKV file recorder",
	.category=MS_FILTER_OTHER,
	.ninputs=2,
	.noutputs=0,
	.init=recorder_init,
	.preprocess=recorder_preprocess,
	.process=recorder_process,
	.postprocess=recorder_postprocess,
	.uninit=recorder_uninit,
	.methods=recorder_methods
};
#endif

MS_FILTER_DESC_EXPORT(ms_mkv_recorder_desc)


/*********************************************************************************************
 * MKV Player Filter                                                                         *
 *********************************************************************************************/
typedef struct {
	Matroska file;
	MSPlayerState state;
	const MSFmtDescriptor **outputDescsList;
	Module **modulesList;
	int *trackNumList;
	ms_bool_t *isFirstFrameList;
	timecode_t time;
} MKVPlayer;

static void player_init(MSFilter *f) {
	MKVPlayer *obj = (MKVPlayer *)ms_new0(MKVPlayer, 1);
	matroska_init(&obj->file);
	obj->state = MSPlayerClosed;
	obj->time = 0;
	obj->outputDescsList = (const MSFmtDescriptor **)ms_new0(const MSFmtDescriptor *, f->desc->noutputs);
	obj->modulesList = (Module **)ms_new0(Module *, f->desc->noutputs);
	obj->trackNumList = (int *)ms_new0(int, f->desc->noutputs);
	obj->isFirstFrameList = (ms_bool_t *)ms_new0(ms_bool_t, f->desc->noutputs);
	f->data = obj;
}

static void player_uninit(MSFilter *f) {
	MKVPlayer *obj = (MKVPlayer *)f->data;
	int i;

	ms_filter_lock(f);
	if(obj->state != MSPlayerClosed) {
		matroska_close_file(&obj->file);
	}
	matroska_uninit(&obj->file);
	for(i=0; i < f->desc->noutputs; i++) {
		if(obj->modulesList[i] != NULL) {
			module_free(obj->modulesList[i]);
		}
	}
	ms_free(obj->outputDescsList);
	ms_free(obj->modulesList);
	ms_free(obj->trackNumList);
	ms_free(obj->isFirstFrameList);
	ms_free(obj);
	ms_filter_unlock(f);
}

static int player_open_file(MSFilter *f, void *arg) {
	MKVPlayer *obj = (MKVPlayer *)f->data;
	const char *filename = (const char *)arg;
	int err = 0;
	int typeList[2] = {TRACK_TYPE_VIDEO, TRACK_TYPE_AUDIO};

	ms_filter_lock(f);
	if(obj->state != MSPlayerClosed) {
		ms_error("MKVPlayer: fail to open %s. A file is already opened", filename);
		err = -1;
	} else {
		if(matroska_open_file(&obj->file, filename, MKV_OPEN_RO) != 0) {
			err = -2;
			ms_error("MKVPlayer: %s could not be opened in read-only mode", filename);
		} else {
			int i;
			for(i=0; i < 2; i++) {
				const char *typeString[2] = {"video", "audio"};
				obj->trackNumList[i] = matroska_get_default_track_num(&obj->file, typeList[i]);
				if(obj->trackNumList[i] <= 0) {
					ms_warning("MKVPlayer: no default %s track. Looking for first %s track", typeString[i], typeString[i]);
					obj->trackNumList[i] = matroska_get_first_track_num_from_type(&obj->file, typeList[i]);
					if(obj->trackNumList[i] <= 0) {
						ms_warning("MKVPlayer: no %s track found", typeString[i]);
					}
				}
				if(obj->trackNumList[i] > 0) {
					const uint8_t *codecPrivateData;
					size_t codecPrivateSize;
					int err;
					if((err = matroska_track_get_info(&obj->file, obj->trackNumList[i], &obj->outputDescsList[i], &codecPrivateData, &codecPrivateSize)) < 0) {
						if(err == -3) {
							ms_warning("MKVPlayer: the %s track info could not be read. Codec ID not supported", typeString[i]);
						} else {
							ms_warning("MKVPlayer: the %s track info could not be read", typeString[i]);
						}
						obj->trackNumList[i] = 0;
					} else {
						obj->modulesList[i] = module_new(obj->outputDescsList[i]->encoding);
						if(obj->modulesList[i] == NULL) {
							ms_error("MKVPlayer: %s is not supported", obj->outputDescsList[i]->encoding);
						} else {
							if(codecPrivateData != NULL) {
								module_load_private_data(obj->modulesList[i], codecPrivateData, codecPrivateSize);
							}
							obj->isFirstFrameList[i] = TRUE;
						}
					}
				}
			}
			if(matroska_block_go_first(&obj->file) < 0) {
				ms_error("MKVPlayer: %s is empty", filename);
				err = -3;
			} else {
				
				obj->state = MSPlayerPaused;
			}
		}
	}
	ms_filter_unlock(f);
	return err;
}

static void player_process(MSFilter *f) {
	int i;
	ms_bool_t eof = FALSE;
	MKVPlayer *obj = (MKVPlayer *)f->data;
	ms_filter_lock(f);
	if(obj->state == MSPlayerPlaying) {
		obj->time += f->ticker->interval;
		while(!eof && matroska_block_get_timestamp(&obj->file) < obj->time) {
			int trackNum;
			mblk_t *frame;
			trackNum = matroska_block_get_track_num(&obj->file);
			for(i=0; i < f->desc->noutputs && obj->trackNumList[i] != trackNum; i++);
			if(i < f->desc->noutputs) {
				frame = matroska_block_read_frame(&obj->file);
				changeClockRate(frame, 1000, obj->outputDescsList[i]->rate);
				module_reverse(obj->modulesList[i], frame, f->outputs[i], obj->isFirstFrameList[i]);
				if(obj->isFirstFrameList[i]) {
					obj->isFirstFrameList[i] = FALSE;
				}
			}
			matroska_block_go_next(&obj->file, &eof);
		}
		if(eof) {
			ms_filter_notify_no_arg(f, MS_PLAYER_EOF);
			matroska_block_go_first(&obj->file);
			obj->state = MSPlayerPaused;
		}
	}
	ms_filter_unlock(f);
}

static int player_close(MSFilter *f, void *arg) {
	int i;
	MKVPlayer *obj = (MKVPlayer *)f->data;
	ms_filter_lock(f);
	if(obj->state != MSPlayerClosed) {
		matroska_close_file(&obj->file);
		for(i=0; i < f->desc->noutputs; i++) {
			if(obj->outputDescsList[i] != NULL) {
				module_free(obj->modulesList[i]);
				obj->modulesList[i] = NULL;
				obj->outputDescsList[i] = NULL;
				obj->trackNumList[i] = 0;
			}
		}
		obj->time = 0;
		obj->state = MSPlayerClosed;
	}
	ms_filter_unlock(f);
	return 0;
}

static int player_start(MSFilter *f, void *arg) {
	MKVPlayer *obj = (MKVPlayer *)f->data;
	int err = 0;
	ms_filter_lock(f);
	if(obj->state == MSPlayerClosed) {
		err = -1;
	} else {
		obj->state = MSPlayerPlaying;
	}
	ms_filter_unlock(f);
	return err;
}

static int player_stop(MSFilter *f, void *arg) {
	MKVPlayer *obj = (MKVPlayer *)f->data;
	ms_filter_lock(f);
	if(obj->state == MSPlayerPlaying) {
		obj->state = MSPlayerPaused;
	}
	ms_filter_unlock(f);
	return 0;
}

static int player_get_output_fmt(MSFilter *f, void *arg) {
	MKVPlayer *obj = (MKVPlayer *)f->data;
	MSPinFormat *pinFmt = (MSPinFormat *)arg;
	int err = 0;
	ms_filter_lock(f);
	if(obj->state == MSPlayerClosed) {
		ms_error("MKVPlayer: cannot get pin format when player is closed");
		err = -1;
	} else {
		if(pinFmt->pin < 0 || pinFmt->pin >= f->desc->noutputs) {
			ms_error("MKVPlayer: pin #%d does not exist", pinFmt->pin);
			err = -2;
		} else {
			pinFmt->fmt = obj->outputDescsList[pinFmt->pin];
		}
	}
	ms_filter_unlock(f);
	return err;
}

static int player_get_state(MSFilter *f, void *arg) {
	MKVPlayer *obj = (MKVPlayer *)f->data;
	MSPlayerState *state = (MSPlayerState *)arg;
	ms_filter_lock(f);
	*state = obj->state;
	ms_filter_unlock(f);
	return 0;
}

static MSFilterMethod player_methods[]= {
	{	MS_FILTER_GET_OUTPUT_FMT	,	player_get_output_fmt	},
	{	MS_PLAYER_OPEN	,	player_open_file	},
	{	MS_PLAYER_CLOSE	,	player_close	},
	{	MS_PLAYER_START	,	player_start	},
	{	MS_PLAYER_PAUSE	,	player_stop	},
	{	MS_PLAYER_GET_STATE	,	player_get_state	},
	{	0	,	NULL	}
};

#ifdef _MSC_VER
MSFilterDesc ms_mkv_player_desc= {
	MS_MKV_PLAYER_ID,
	"MSMKVPlayer",
	"MKV file player",
	MS_FILTER_OTHER,
	0,
	2,
	player_init,
	NULL,
	player_process,
	NULL,
	player_uninit,
	player_methods
};
#else
MSFilterDesc ms_mkv_player_desc={
	.id=MS_MKV_PLAYER_ID,
	.name="MSMKVPlayer",
	.text="MKV file player",
	.category=MS_FILTER_OTHER,
	.ninputs=0,
	.noutputs=2,
	.init=player_init,
	.preprocess=NULL,
	.process=player_process,
	.postprocess=NULL,
	.uninit=player_uninit,
	.methods=player_methods
};
#endif

MS_FILTER_DESC_EXPORT(ms_mkv_player_desc)