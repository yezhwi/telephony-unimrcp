/*
 * Copyright 2008 Arsen Chaloyan
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* 
 * Some mandatory rules concerning plugin implementation.
 * 1. Each plugin MUST contain the following function as an entry point of the plugin
 *        MRCP_PLUGIN_DECLARE(mrcp_resource_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
 * 2. One and only one response MUST be sent back to the received request.
 * 3. Methods (callbacks) of the MRCP engine channel MUST not block.
 *   (asynch response can be sent from the context of other thread)
 * 4. Methods (callbacks) of the MPF engine stream MUST not block.
 */

#include <swift.h>
#include "mrcp_resource_engine.h"
#include "mrcp_synth_resource.h"
#include "mrcp_synth_header.h"
#include "mrcp_generic_header.h"
#include "mrcp_message.h"
#include "mpf_buffer.h"
#include "apt_log.h"

typedef struct mrcp_swift_channel_t mrcp_swift_channel_t;

/** Declaration of synthesizer engine methods */
static apt_bool_t mrcp_swift_engine_destroy(mrcp_resource_engine_t *engine);
static apt_bool_t mrcp_swift_engine_open(mrcp_resource_engine_t *engine);
static apt_bool_t mrcp_swift_engine_close(mrcp_resource_engine_t *engine);
static mrcp_engine_channel_t* mrcp_swift_engine_channel_create(mrcp_resource_engine_t *engine, apr_pool_t *pool);

static const struct mrcp_engine_method_vtable_t engine_vtable = {
	mrcp_swift_engine_destroy,
	mrcp_swift_engine_open,
	mrcp_swift_engine_close,
	mrcp_swift_engine_channel_create
};


/** Declaration of synthesizer channel methods */
static apt_bool_t mrcp_swift_channel_destroy(mrcp_engine_channel_t *channel);
static apt_bool_t mrcp_swift_channel_open(mrcp_engine_channel_t *channel);
static apt_bool_t mrcp_swift_channel_close(mrcp_engine_channel_t *channel);
static apt_bool_t mrcp_swift_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request);

static const struct mrcp_engine_channel_method_vtable_t channel_vtable = {
	mrcp_swift_channel_destroy,
	mrcp_swift_channel_open,
	mrcp_swift_channel_close,
	mrcp_swift_channel_request_process
};

/** Declaration of synthesizer audio stream methods */
static apt_bool_t synth_stream_destroy(mpf_audio_stream_t *stream);
static apt_bool_t synth_stream_open(mpf_audio_stream_t *stream);
static apt_bool_t synth_stream_close(mpf_audio_stream_t *stream);
static apt_bool_t synth_stream_read(mpf_audio_stream_t *stream, mpf_frame_t *frame);

static const mpf_audio_stream_vtable_t audio_stream_vtable = {
	synth_stream_destroy,
	synth_stream_open,
	synth_stream_close,
	synth_stream_read,
	NULL,
	NULL,
	NULL
};

/** Declaration of swift synthesizer channel */
struct mrcp_swift_channel_t {
	/** Swift port */
	swift_port            *port;
	swift_background_t     tts_stream;

	/** Audio buffer */
	mpf_buffer_t          *audio_buffer;
	float                  last_write_time;

	/** Engine channel base */
	mrcp_engine_channel_t *channel;

	/** Active (in-progress) speak request */
	mrcp_message_t        *speak_request;
	/** Pending stop response */
	mrcp_message_t        *stop_response;
	/** Is paused */
	apt_bool_t             paused;
};

static void swift_voices_show(swift_engine *engine);
static swift_result_t swift_write_audio(swift_event *event, swift_event_t type, void *udata);
static apt_bool_t synth_speak_complete_raise(mrcp_swift_channel_t *synth_channel);

/** Create swift synthesizer engine */
MRCP_PLUGIN_DECLARE(mrcp_resource_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
{
	swift_engine *engine = NULL;
	apt_log_priority_set(APT_PRIO_DEBUG);
	/* open swift engine */
	apt_log(APT_PRIO_INFO,"Open Swift Engine");
	if((engine = swift_engine_open(NULL)) ==  NULL) {
		apt_log(APT_PRIO_WARNING,"Failed to Open Swift Engine");
		return NULL;
	}
	swift_voices_show(engine);

	/* create resource engine base */
	return mrcp_resource_engine_create(
					MRCP_SYNTHESIZER_RESOURCE, /* MRCP resource identifier */
					engine,                    /* object to associate */
					&engine_vtable,            /* virtual methods table of resource engine */
					pool);                     /* pool to allocate memory from */
}

/** Destroy synthesizer engine */
static apt_bool_t mrcp_swift_engine_destroy(mrcp_resource_engine_t *engine)
{
	swift_engine *mrcp_swift_engine = engine->obj;
	/* close swift engine */
	apt_log(APT_PRIO_INFO,"Close Swift Engine");
	if(NULL != mrcp_swift_engine) {
		swift_engine_close(mrcp_swift_engine);
		engine->obj = NULL;
	}
	return TRUE;
}

/** Open synthesizer engine */
static apt_bool_t mrcp_swift_engine_open(mrcp_resource_engine_t *engine)
{
	return TRUE;
}

/** Close synthesizer engine */
static apt_bool_t mrcp_swift_engine_close(mrcp_resource_engine_t *engine)
{
	return TRUE;
}

/** Create demo synthesizer channel derived from engine channel base */
static mrcp_engine_channel_t* mrcp_swift_engine_channel_create(mrcp_resource_engine_t *engine, apr_pool_t *pool)
{
	swift_engine *synth_engine = engine->obj;
	mrcp_swift_channel_t *synth_channel;
	mrcp_engine_channel_t *channel;
	swift_params *params;
	swift_port *port;
	mpf_codec_descriptor_t *codec_descriptor;
	mpf_codec_t *codec;

	codec_descriptor = apr_palloc(pool,sizeof(mpf_codec_descriptor_t));
	mpf_codec_descriptor_init(codec_descriptor);
	codec_descriptor->channel_count = 1;
	codec_descriptor->payload_type = 96;
	apt_string_set(&codec_descriptor->name,"L16");
	codec_descriptor->sampling_rate = 8000;

	params = swift_params_new(NULL);
	swift_params_set_string(params, "audio/encoding", "pcm16");
	swift_params_set_int(params, "audio/sampling-rate", codec_descriptor->sampling_rate);
	/* open swift port */ 
	apt_log(APT_PRIO_INFO,"Open Swift Port");
	if((port = swift_port_open(synth_engine,params)) == NULL) {
		apt_log(APT_PRIO_WARNING,"Failed to Open Swift Port");
		return NULL;
	}

	/* create swift synth channel */
	synth_channel = apr_palloc(pool,sizeof(mrcp_swift_channel_t));
	synth_channel->speak_request = NULL;
	synth_channel->stop_response = NULL;
	synth_channel->paused = FALSE;
	synth_channel->channel = NULL;
	synth_channel->port = port;
	synth_channel->tts_stream = 0;
	/* create engine channel base */
	channel = mrcp_engine_source_channel_create(
			engine,               /* resource engine */
			&channel_vtable,      /* virtual methods table of engine channel */
			&audio_stream_vtable, /* virtual methods table of audio stream */
			synth_channel,        /* object to associate */
			codec_descriptor,     /* codec descriptor might be NULL by default */
			pool);                /* pool to allocate memory from */

	codec = mrcp_engine_source_stream_codec_get(channel);

	if(!channel || !codec) {
		swift_port_close(port);
		synth_channel->port = NULL;
		return NULL;
	}

	synth_channel->audio_buffer = mpf_buffer_create(pool);
	synth_channel->last_write_time = 0;

	/* set swift_write_audio as a callback, with the output file as its param */
	swift_port_set_callback(port, &swift_write_audio, SWIFT_EVENT_AUDIO | SWIFT_EVENT_END, synth_channel);
	synth_channel->channel = channel;
	return channel;
}

/** Destroy engine channel */
static apt_bool_t mrcp_swift_channel_destroy(mrcp_engine_channel_t *channel)
{
	mrcp_swift_channel_t *synth_channel = channel->method_obj;
	apt_log(APT_PRIO_INFO,"Close Swift Port");
	if(NULL != synth_channel->port) {
		/* close swift port */ 
		swift_port_close(synth_channel->port);
		synth_channel->port = NULL;
	}
	return TRUE;
}

/** Open engine channel (asynchronous response MUST be sent)*/
static apt_bool_t mrcp_swift_channel_open(mrcp_engine_channel_t *channel)
{
	/* open channel and send asynch response */
	return mrcp_engine_channel_open_respond(channel,TRUE);
}

/** Close engine channel (asynchronous response MUST be sent)*/
static apt_bool_t mrcp_swift_channel_close(mrcp_engine_channel_t *channel)
{
	/* close channel, make sure there is no activity and send asynch response */
	return mrcp_engine_channel_close_respond(channel);
}

/** Process SPEAK request */
static apt_bool_t mrcp_swift_channel_speak(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	mrcp_swift_channel_t *synth_channel = channel->method_obj;

	mpf_buffer_restart(synth_channel->audio_buffer);
	response->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	if(swift_port_speak_text(synth_channel->port,request->body.buf,0,NULL,&synth_channel->tts_stream,NULL) != SWIFT_SUCCESS) {
		response->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;
		response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
	}
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	synth_channel->speak_request = request;
	return TRUE;
}

/** Process STOP request */
static apt_bool_t mrcp_swift_channel_stop(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	mrcp_swift_channel_t *synth_channel = channel->method_obj;
	/* store the request, make sure there is no more activity and only then send the response */
	swift_port_stop(synth_channel->port,synth_channel->tts_stream,SWIFT_EVENT_NOW);
	synth_channel->stop_response = response;
	return TRUE;
}

/** Process PAUSE request */
static apt_bool_t mrcp_swift_channel_pause(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	mrcp_swift_channel_t *synth_channel = channel->method_obj;
	synth_channel->paused = TRUE;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/** Process RESUME request */
static apt_bool_t mrcp_swift_channel_resume(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	mrcp_swift_channel_t *synth_channel = channel->method_obj;
	synth_channel->paused = FALSE;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/** Process MRCP channel request (asynchronous response MUST be sent)*/
static apt_bool_t mrcp_swift_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t processed = FALSE;
	mrcp_message_t *response = mrcp_response_create(request,request->pool);
	switch(request->start_line.method_id) {
		case SYNTHESIZER_SET_PARAMS:
			break;
		case SYNTHESIZER_GET_PARAMS:
			break;
		case SYNTHESIZER_SPEAK:
			processed = mrcp_swift_channel_speak(channel,request,response);
			break;
		case SYNTHESIZER_STOP:
			processed = mrcp_swift_channel_stop(channel,request,response);
			break;
		case SYNTHESIZER_PAUSE:
			processed = mrcp_swift_channel_pause(channel,request,response);
			break;
		case SYNTHESIZER_RESUME:
			processed = mrcp_swift_channel_resume(channel,request,response);
			break;
		case SYNTHESIZER_BARGE_IN_OCCURRED:
			processed = mrcp_swift_channel_stop(channel,request,response);
			break;
		case SYNTHESIZER_CONTROL:
			break;
		case SYNTHESIZER_DEFINE_LEXICON:
			break;
		default:
			break;
	}
	if(processed == FALSE) {
		/* send asynchronous response for not handled request */
		mrcp_engine_channel_message_send(channel,response);
	}
	return TRUE;
}



/** Callback is called from MPF engine context to destroy any additional data associated with audio stream */
static apt_bool_t synth_stream_destroy(mpf_audio_stream_t *stream)
{
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action before open */
static apt_bool_t synth_stream_open(mpf_audio_stream_t *stream)
{
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action after close */
static apt_bool_t synth_stream_close(mpf_audio_stream_t *stream)
{
	return TRUE;
}

/** Callback is called from MPF engine context to read/get new frame */
static apt_bool_t synth_stream_read(mpf_audio_stream_t *stream, mpf_frame_t *frame)
{
	mrcp_swift_channel_t *synth_channel = stream->obj;
	/* check if STOP was requested */
	if(synth_channel->stop_response) {
		/* send asynchronous response to STOP request */
		mrcp_engine_channel_message_send(synth_channel->channel,synth_channel->stop_response);
		synth_channel->stop_response = NULL;
		synth_channel->speak_request = NULL;
		synth_channel->paused = FALSE;
		return TRUE;
	}

	/* check if there is active SPEAK request and it isn't in paused state */
	if(synth_channel->speak_request && synth_channel->paused == FALSE) {
		/* normal processing */
		mpf_buffer_frame_read(synth_channel->audio_buffer,frame);
		
		if((frame->type & MEDIA_FRAME_TYPE_EVENT) == MEDIA_FRAME_TYPE_EVENT) {
			synth_speak_complete_raise(synth_channel);
		}
	}
	return TRUE;
}

/** Swift engine callback */
static swift_result_t swift_write_audio(swift_event *event, swift_event_t type, void *udata)
{
	void *buf;
	int len;
	mrcp_swift_channel_t *synth_channel = udata;
	swift_event_t rv = SWIFT_SUCCESS;

	if(type & SWIFT_EVENT_END) {
		apt_log(APT_PRIO_INFO,"Swift Event: end\n");
		mpf_buffer_event_write(synth_channel->audio_buffer,MEDIA_FRAME_TYPE_EVENT);
		return rv;
	}

	rv = swift_event_get_audio(event, &buf, &len);
	if(!SWIFT_FAILED(rv)) {
		/* Get the event times */
		float time_start, time_len; 
		swift_event_get_times(event, &time_start, &time_len);
		apt_log(APT_PRIO_DEBUG,"Swift Engine: Write Audio [%d | %0.4f | %0.4f]",len, time_start, time_len);
		synth_channel->last_write_time = time_start + time_len;
		mpf_buffer_audio_write(synth_channel->audio_buffer,buf,len);
	}

	return rv;
}

/** Raise SPEAK-COMPLETE event */
static apt_bool_t synth_speak_complete_raise(mrcp_swift_channel_t *synth_channel)
{
	mrcp_message_t *message;
	mrcp_synth_header_t *synth_header;
	if(!synth_channel->speak_request) {
		return FALSE;
	}
	message = mrcp_event_create(
						synth_channel->speak_request,
						SYNTHESIZER_SPEAK_COMPLETE,
						synth_channel->speak_request->pool);
	if(!message) {
		return FALSE;
	}
	
	/* get/allocate synthesizer header */
	synth_header = mrcp_resource_header_prepare(message);
	if(synth_header) {
		/* set completion cause */
		synth_header->completion_cause = SYNTHESIZER_COMPLETION_CAUSE_NORMAL;
		mrcp_resource_header_property_add(message,SYNTHESIZER_HEADER_COMPLETION_CAUSE);
	}
	/* set request state */
	message->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;

	synth_channel->speak_request = NULL;
	/* send asynch event */
	return mrcp_engine_channel_message_send(synth_channel->channel,message);
}

/** Show swift available voices */
static void swift_voices_show(swift_engine *engine) 
{
	swift_port *port;
	swift_voice *voice;
	const char *license_status;

	/* open swift port*/
	if((port = swift_port_open(engine, NULL)) == NULL) {
		apt_log(APT_PRIO_WARNING,"Failed to Open Swift Port");
		return;    
	}

	/* find the first voice on the system */
	if((voice = swift_port_find_first_voice(port, NULL, NULL)) == NULL) {
		apt_log(APT_PRIO_WARNING,"Failed to Find Any Voices!");
		swift_port_close(port);
		return;
	}
	/* go through all of the voices on the system and print some info about each */
	apt_log(APT_PRIO_INFO,"Swift Available Voices:");
	for(; voice; voice = swift_port_find_next_voice(port)) {
		if(swift_voice_get_attribute(voice, "license/key")) {
			license_status = "licensed";
		}
		else {
			license_status = "unlicensed";
		}
		apt_log(APT_PRIO_INFO,"%s: %s, age %s, %s, %sHz, %s",
			swift_voice_get_attribute(voice, "name"),
			swift_voice_get_attribute(voice, "speaker/gender"),
			swift_voice_get_attribute(voice, "speaker/age"),
			swift_voice_get_attribute(voice, "language/name"),
			swift_voice_get_attribute(voice, "sample-rate"),
			license_status);
	}

	swift_port_close(port);
}
