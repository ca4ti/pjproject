/* $Id$ */
/*
 * Copyright (C) 2012-2012 Teluu Inc. (http://www.teluu.com)
 * Copyright (C) 2010-2012 Regis Montoya (aka r3gis - www.r3gis.fr)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


/* This file is the implementation of Android Oboe audio device. */

#include <pjmedia-audiodev/audiodev_imp.h>
#include <pj/assert.h>
#include <pj/lock.h>
#include <pj/log.h>
#include <pj/os.h>
#include <pjmedia/circbuf.h>
#include <pjmedia/errno.h>

#if defined(PJMEDIA_AUDIO_DEV_HAS_OBOE) &&  PJMEDIA_AUDIO_DEV_HAS_OBOE != 0

#define THIS_FILE	"oboe_dev.cpp"
#define DRIVER_NAME	"Oboe"

#include <semaphore.h>
#include <android/log.h>
#include <oboe/Oboe.h>

struct oboe_aud_factory
{
    pjmedia_aud_dev_factory base;
    pj_pool_factory        *pf;
    pj_pool_t              *pool;
};

class MyOboeEngine;

/*
 * Sound stream descriptor.
 * This struct may be used for both unidirectional or bidirectional sound
 * streams.
 */
struct oboe_aud_stream
{
    pjmedia_aud_stream  base;
    pj_pool_t          *pool;
    pj_str_t            name;
    pjmedia_dir         dir;
    pjmedia_aud_param   param;

    int                 bytes_per_sample;
    pj_uint32_t         samples_per_sec;
    unsigned            samples_per_frame;
    int                 channel_count;
    void               *user_data;
    pj_bool_t           running;

    /* Capture/record */
    MyOboeEngine       *rec_engine;
    pjmedia_aud_rec_cb  rec_cb;

    /* Playback */
    MyOboeEngine       *play_engine;
    pjmedia_aud_play_cb play_cb;
};

/* Factory prototypes */
static pj_status_t oboe_init(pjmedia_aud_dev_factory *f);
static pj_status_t oboe_destroy(pjmedia_aud_dev_factory *f);
static pj_status_t oboe_refresh(pjmedia_aud_dev_factory *f);
static unsigned oboe_get_dev_count(pjmedia_aud_dev_factory *f);
static pj_status_t oboe_get_dev_info(pjmedia_aud_dev_factory *f,
                                        unsigned index,
                                        pjmedia_aud_dev_info *info);
static pj_status_t oboe_default_param(pjmedia_aud_dev_factory *f,
                                         unsigned index,
                                         pjmedia_aud_param *param);
static pj_status_t oboe_create_stream(pjmedia_aud_dev_factory *f,
                                         const pjmedia_aud_param *param,
                                         pjmedia_aud_rec_cb rec_cb,
                                         pjmedia_aud_play_cb play_cb,
                                         void *user_data,
                                         pjmedia_aud_stream **p_aud_strm);

/* Stream prototypes */
static pj_status_t strm_get_param(pjmedia_aud_stream *strm,
                                  pjmedia_aud_param *param);
static pj_status_t strm_get_cap(pjmedia_aud_stream *strm,
                                pjmedia_aud_dev_cap cap,
                                void *value);
static pj_status_t strm_set_cap(pjmedia_aud_stream *strm,
                                pjmedia_aud_dev_cap cap,
                                const void *value);
static pj_status_t strm_start(pjmedia_aud_stream *strm);
static pj_status_t strm_stop(pjmedia_aud_stream *strm);
static pj_status_t strm_destroy(pjmedia_aud_stream *strm);

static pjmedia_aud_dev_factory_op oboe_op =
{
    &oboe_init,
    &oboe_destroy,
    &oboe_get_dev_count,
    &oboe_get_dev_info,
    &oboe_default_param,
    &oboe_create_stream,
    &oboe_refresh
};

static pjmedia_aud_stream_op oboe_strm_op =
{
    &strm_get_param,
    &strm_get_cap,
    &strm_set_cap,
    &strm_start,
    &strm_stop,
    &strm_destroy
};


/*
 * Init Android Oboe audio driver.
 */
#ifdef __cplusplus
extern "C"{
#endif
pjmedia_aud_dev_factory* pjmedia_android_oboe_factory(pj_pool_factory *pf)
{
    struct oboe_aud_factory *f;
    pj_pool_t *pool;

    pool = pj_pool_create(pf, "oboe", 256, 256, NULL);
    f = PJ_POOL_ZALLOC_T(pool, struct oboe_aud_factory);
    f->pf = pf;
    f->pool = pool;
    f->base.op = &oboe_op;

    return &f->base;
}
#ifdef __cplusplus
}
#endif


/* API: Init factory */
static pj_status_t oboe_init(pjmedia_aud_dev_factory *f)
{
    PJ_UNUSED_ARG(f);

    PJ_LOG(4, (THIS_FILE, "Oboe sound library initialized"));

    return PJ_SUCCESS;
}


/* API: refresh the list of devices */
static pj_status_t oboe_refresh(pjmedia_aud_dev_factory *f)
{
    PJ_UNUSED_ARG(f);
    return PJ_SUCCESS;
}


/* API: Destroy factory */
static pj_status_t oboe_destroy(pjmedia_aud_dev_factory *f)
{
    struct oboe_aud_factory *pa = (struct oboe_aud_factory*)f;
    pj_pool_t *pool;

    PJ_LOG(4, (THIS_FILE, "Oboe sound library shutting down.."));

    pool = pa->pool;
    pa->pool = NULL;
    pj_pool_release(pool);

    return PJ_SUCCESS;
}

/* API: Get device count. */
static unsigned oboe_get_dev_count(pjmedia_aud_dev_factory *f)
{
    PJ_UNUSED_ARG(f);
    return 1;
}

/* API: Get device info. */
static pj_status_t oboe_get_dev_info(pjmedia_aud_dev_factory *f,
                                        unsigned index,
                                        pjmedia_aud_dev_info *info)
{
    PJ_UNUSED_ARG(f);

    pj_bzero(info, sizeof(*info));

    pj_ansi_strcpy(info->name, "Oboe");
    info->default_samples_per_sec = 8000;
    info->caps = PJMEDIA_AUD_DEV_CAP_OUTPUT_VOLUME_SETTING |
    		 PJMEDIA_AUD_DEV_CAP_INPUT_SOURCE;
    info->input_count = 1;
    info->output_count = 1;
    info->routes = PJMEDIA_AUD_DEV_ROUTE_CUSTOM;

    return PJ_SUCCESS;
}

/* API: fill in with default parameter. */
static pj_status_t oboe_default_param(pjmedia_aud_dev_factory *f,
                                         unsigned index,
                                         pjmedia_aud_param *param)
{
    pjmedia_aud_dev_info adi;
    pj_status_t status;

    status = oboe_get_dev_info(f, index, &adi);
    if (status != PJ_SUCCESS)
	return status;

    pj_bzero(param, sizeof(*param));
    if (adi.input_count && adi.output_count) {
        param->dir = PJMEDIA_DIR_CAPTURE_PLAYBACK;
        param->rec_id = index;
        param->play_id = index;
    } else if (adi.input_count) {
        param->dir = PJMEDIA_DIR_CAPTURE;
        param->rec_id = index;
        param->play_id = PJMEDIA_AUD_INVALID_DEV;
    } else if (adi.output_count) {
        param->dir = PJMEDIA_DIR_PLAYBACK;
        param->play_id = index;
        param->rec_id = PJMEDIA_AUD_INVALID_DEV;
    } else {
        return PJMEDIA_EAUD_INVDEV;
    }

    param->clock_rate = adi.default_samples_per_sec;
    param->channel_count = 1;
    param->samples_per_frame = adi.default_samples_per_sec * 20 / 1000;
    param->bits_per_sample = 16;
    param->input_latency_ms = PJMEDIA_SND_DEFAULT_REC_LATENCY;
    param->output_latency_ms = PJMEDIA_SND_DEFAULT_PLAY_LATENCY;

    return PJ_SUCCESS;
}


class MyOboeEngine : oboe::AudioStreamDataCallback {
public:
    MyOboeEngine(struct oboe_aud_stream *stream_, pjmedia_dir dir_)
    : stream(stream_), dir(dir_), oboe_stream(NULL), dir_st(NULL),
      thread(NULL), thread_quit(PJ_FALSE), buf(NULL)
    {
	pj_assert(dir == PJMEDIA_DIR_CAPTURE || dir == PJMEDIA_DIR_PLAYBACK);
	dir_st = (dir == PJMEDIA_DIR_CAPTURE? "capture":"playback");
	pj_set_timestamp32(&ts, 0, 0);
    }
    
    pj_status_t Start() {
	if (oboe_stream)
	    return PJ_SUCCESS;
    
	oboe::AudioStreamBuilder sb;
	pj_status_t status;

	if (dir == PJMEDIA_DIR_CAPTURE) {
	    sb.setDirection(oboe::Direction::Input);
	    sb.setDeviceId(stream->param.rec_id);
	} else {
	    sb.setDirection(oboe::Direction::Output);
	    sb.setDeviceId(stream->param.play_id);
	}
	sb.setSampleRate(stream->param.clock_rate);
	sb.setChannelCount(stream->param.channel_count);
	sb.setSharingMode(oboe::SharingMode::Exclusive);
	sb.setFormat(oboe::AudioFormat::I16);
	sb.setDataCallback(this);
	sb.setFramesPerDataCallback(stream->param.samples_per_frame /
				    stream->param.channel_count);

	/* Somehow mic does not work if low latency is specified */
	if (dir != PJMEDIA_DIR_CAPTURE)
	    sb.setPerformanceMode(oboe::PerformanceMode::LowLatency);

	/* Let Oboe do the resampling to minimize latency */
	sb.setSampleRateConversionQuality(
				oboe::SampleRateConversionQuality::High);

	/* Create buffer and its lock */
	status = pjmedia_circ_buf_create(stream->pool,
					 stream->param.samples_per_frame * 3,
					 &buf);
        if (status != PJ_SUCCESS)
            return status;

	if (pthread_mutex_init(&buf_lock, NULL) != 0) {
	    return PJ_RETURN_OS_ERROR(pj_get_native_os_error());
	}

	/* Create semaphore */
        if (sem_init(&sem, 0, 0) != 0) {
	    return PJ_RETURN_OS_ERROR(pj_get_native_os_error());
	}

	/* Create thread */
	thread_quit = PJ_FALSE;
        status = pj_thread_create(stream->pool, "android_oboe",
                                  AudioThread, this, 0, 0, &thread);
        if (status != PJ_SUCCESS)
            return status;    

	/* Open & start oboe stream */
	oboe::Result result = sb.openStream(&oboe_stream);
	if (result != oboe::Result::OK) {
	    PJ_LOG(3,(THIS_FILE,
		      "Oboe stream %s open failed (err=%d/%s)",
		      dir_st, result, oboe::convertToText(result)));
	    return PJMEDIA_EAUD_SYSERR;
	}

	result = oboe_stream->requestStart();
	if (result != oboe::Result::OK) {
	    PJ_LOG(3,(THIS_FILE,
		      "Oboe stream %s start failed (err=%d/%s)",
		      dir_st, result, oboe::convertToText(result)));
	    return PJMEDIA_EAUD_SYSERR;
	}

	// TODO: more attributes in log, e.g: sharing mode, performance mode,
	// buffer capacity in frames, audio api, etc
	PJ_LOG(4, (THIS_FILE, 
	       "Oboe stream %s started, "
	       "id=%d, clock_rate=%d, "
	       "channel_count=%d, samples_per_frame=%d (%dms)",
	       dir_st,
	       stream->param.play_id,
	       stream->param.clock_rate,
	       stream->param.channel_count,
	       stream->param.samples_per_frame,
	       stream->param.samples_per_frame * 1000 /
		       stream->param.clock_rate));

	return PJ_SUCCESS;
    }

    void Stop() {
	if (oboe_stream) {
	    oboe_stream->close();
	    delete oboe_stream;
	    oboe_stream = NULL;
	}

	if (thread) {
	    thread_quit = PJ_TRUE;
            sem_post(&sem);
            pj_thread_join(thread);
            pj_thread_destroy(thread);
            thread = NULL;
            sem_destroy(&sem);
	    pthread_mutex_destroy(&buf_lock);
	}

	PJ_LOG(4, (THIS_FILE, "Oboe stream %s stopped.", dir_st));
    }

    /* Oboe callback, here let's just use Android native mutex & semaphore
     * so we don't need to register the thread to PJLIB.
     */
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *oboeStream,
					  void *audioData,
					  int32_t numFrames)
    {
	pj_status_t status;

	pthread_mutex_lock(&buf_lock);
	if (dir == PJMEDIA_DIR_CAPTURE) {
	    /* Write audio data to circular buffer */
	    status = pjmedia_circ_buf_write(buf, (pj_int16_t*)audioData,
				numFrames * stream->param.channel_count);
	    if (status != PJ_SUCCESS) {
		__android_log_write(ANDROID_LOG_WARN, THIS_FILE,
			"Oboe capture failed writing to circ buffer");
	    }
	} else {
	    /* Read audio data from circular buffer */
	    status = pjmedia_circ_buf_read(buf, (pj_int16_t*)audioData,
				numFrames * stream->param.channel_count);
	    if (status != PJ_SUCCESS) {
		__android_log_write(ANDROID_LOG_WARN, THIS_FILE,
			"Oboe playback failed reading from circ buffer");
	    }
	}
	pthread_mutex_unlock(&buf_lock);

	sem_post(&sem);

	return oboe::DataCallbackResult::Continue;
    }
    
    ~MyOboeEngine() {
	Stop();
    }

private:
    
    static int AudioThread(void *arg) {
	MyOboeEngine *this_ = (MyOboeEngine*)arg;
	struct oboe_aud_stream *stream = this_->stream;
	pj_int16_t *tmp_buf;
	unsigned ts_inc;
	pj_status_t status;

	/* Try to bump up the thread priority */
	enum {
	    THREAD_PRIORITY_AUDIO = -16,
	    THREAD_PRIORITY_URGENT_AUDIO = -19
	};
	status = pj_thread_set_prio(NULL, THREAD_PRIORITY_URGENT_AUDIO);
	if (status != PJ_SUCCESS) {
	    PJ_PERROR(3,(THIS_FILE, status,
			 "Warning: Oboe %s failed increasing thread priority",
			 this_->dir_st));
	}

	tmp_buf = new pj_int16_t[this_->stream->param.samples_per_frame];
	ts_inc = stream->param.samples_per_frame/stream->param.channel_count;
	while (1) {
	    sem_wait(&this_->sem);;
	    if (this_->thread_quit)
		break;

	    if (this_->dir == PJMEDIA_DIR_CAPTURE) {
		/* Read audio frame from Oboe via circular buffer */
		pthread_mutex_lock(&this_->buf_lock);
		status = pjmedia_circ_buf_read(this_->buf, tmp_buf,
					       stream->param.samples_per_frame);
		if (status != PJ_SUCCESS) {
		    PJ_PERROR(4,(THIS_FILE, status,
				 "Oboe %s failed reading from circ buffer",
				 this_->dir_st));
		}
		pthread_mutex_unlock(&this_->buf_lock);
		if (status != PJ_SUCCESS)
		    continue;

		/* Send audio frame to app via callback rec_cb() */
		pjmedia_frame frame;
		frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
		frame.size =  stream->param.samples_per_frame * 2;
		frame.bit_info = 0;
		frame.buf = (void *)tmp_buf;
		frame.timestamp = this_->ts;
		status = (*stream->rec_cb)(stream->user_data, &frame);
		if (status != PJ_SUCCESS) {
		    /* App wants to stop audio dev stream */
		    break;
		}
	    } else {
		/* Get audio frame from app via callback play_cb() */
		pjmedia_frame frame;
		frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
		frame.size =  stream->param.samples_per_frame * 2;
		frame.bit_info = 0;
		frame.buf = (void *)tmp_buf;
		frame.timestamp = this_->ts;
		status = (*stream->play_cb)(stream->user_data, &frame);

		/* Send audio frame to Oboe via circular buffer */
		if (status == PJ_SUCCESS) {
		    pthread_mutex_lock(&this_->buf_lock);
		    status = pjmedia_circ_buf_write(this_->buf, tmp_buf,
					stream->param.samples_per_frame);
		    if (status != PJ_SUCCESS) {
		        PJ_PERROR(4,(THIS_FILE, status,
				     "Oboe %s failed writing to circ buffer",
				     this_->dir_st));
		    }
		    pthread_mutex_unlock(&this_->buf_lock);
		} else {
		    /* App wants to stop audio dev stream */
		    break;
		}
	    }

	    /* Increment timestamp */
	    pj_add_timestamp32(&this_->ts, ts_inc);
	}

	delete [] tmp_buf;
	return 0;
    }

private:
    struct oboe_aud_stream	*stream;
    pjmedia_dir			 dir;
    oboe::AudioStream		*oboe_stream;
    const char			*dir_st;
    pj_thread_t			*thread;
    pj_bool_t			 thread_quit;
    sem_t			 sem;
    pjmedia_circ_buf		*buf;
    pthread_mutex_t		 buf_lock;
    pj_timestamp		 ts;
};


/* API: create stream */
static pj_status_t oboe_create_stream(pjmedia_aud_dev_factory *f,
				      const pjmedia_aud_param *param,
				      pjmedia_aud_rec_cb rec_cb,
				      pjmedia_aud_play_cb play_cb,
				      void *user_data,
				      pjmedia_aud_stream **p_aud_strm)
{
    struct oboe_aud_factory *pa = (struct oboe_aud_factory*)f;
    pj_pool_t *pool;
    struct oboe_aud_stream *stream;
    pj_status_t status = PJ_SUCCESS;

    PJ_ASSERT_RETURN(param->channel_count >= 1 && param->channel_count <= 2,
                     PJ_EINVAL);
    PJ_ASSERT_RETURN(param->bits_per_sample==8 || param->bits_per_sample==16,
                     PJ_EINVAL);
    PJ_ASSERT_RETURN(play_cb && rec_cb && p_aud_strm, PJ_EINVAL);

    pool = pj_pool_create(pa->pf, "oboestrm", 1024, 1024, NULL);
    if (!pool)
        return PJ_ENOMEM;

    PJ_LOG(4, (THIS_FILE, "Creating Oboe stream"));

    stream = PJ_POOL_ZALLOC_T(pool, struct oboe_aud_stream);
    stream->pool = pool;
    pj_strdup2_with_null(pool, &stream->name, "Oboe stream");
    stream->dir = param->dir;
    pj_memcpy(&stream->param, param, sizeof(*param));
    stream->user_data = user_data;
    stream->rec_cb = rec_cb;
    stream->play_cb = play_cb;

    if (param->dir & PJMEDIA_DIR_CAPTURE) {
	stream->rec_engine = new MyOboeEngine(stream, PJMEDIA_DIR_CAPTURE);
	if (!stream->rec_engine) {
	    status = PJ_ENOMEM;
	    goto on_error;
	}
    }

    if (stream->dir & PJMEDIA_DIR_PLAYBACK) {
	stream->play_engine = new MyOboeEngine(stream, PJMEDIA_DIR_PLAYBACK);
	if (!stream->play_engine) {
	    status = PJ_ENOMEM;
	    goto on_error;
	}
    }

    /* Done */
    stream->base.op = &oboe_strm_op;
    *p_aud_strm = &stream->base;

    return PJ_SUCCESS;

on_error:
    strm_destroy(&stream->base);
    return status;
}

/* API: Get stream parameters */
static pj_status_t strm_get_param(pjmedia_aud_stream *s,
                                  pjmedia_aud_param *pi)
{
    struct oboe_aud_stream *strm = (struct oboe_aud_stream*)s;
    PJ_ASSERT_RETURN(strm && pi, PJ_EINVAL);
    pj_memcpy(pi, &strm->param, sizeof(*pi));

    if (strm_get_cap(s, PJMEDIA_AUD_DEV_CAP_OUTPUT_VOLUME_SETTING,
                     &pi->output_vol) == PJ_SUCCESS)
    {
        pi->flags |= PJMEDIA_AUD_DEV_CAP_OUTPUT_VOLUME_SETTING;
    }

    return PJ_SUCCESS;
}

/* API: get capability */
static pj_status_t strm_get_cap(pjmedia_aud_stream *s,
                                pjmedia_aud_dev_cap cap,
                                void *pval)
{
    struct oboe_aud_stream *strm = (struct oboe_aud_stream*)s;
    pj_status_t status = PJMEDIA_EAUD_INVCAP;

    PJ_ASSERT_RETURN(s && pval, PJ_EINVAL);

    if (cap==PJMEDIA_AUD_DEV_CAP_OUTPUT_VOLUME_SETTING &&
	(strm->param.dir & PJMEDIA_DIR_PLAYBACK))
    {
    }

    return status;
}

/* API: set capability */
static pj_status_t strm_set_cap(pjmedia_aud_stream *s,
                                pjmedia_aud_dev_cap cap,
                                const void *value)
{
    struct oboe_aud_stream *stream = (struct oboe_aud_stream*)s;

    PJ_ASSERT_RETURN(s && value, PJ_EINVAL);

    if (cap==PJMEDIA_AUD_DEV_CAP_OUTPUT_VOLUME_SETTING &&
	(stream->param.dir & PJMEDIA_DIR_PLAYBACK))
    {
    }

    return PJMEDIA_EAUD_INVCAP;
}

/* API: start stream. */
static pj_status_t strm_start(pjmedia_aud_stream *s)
{
    struct oboe_aud_stream *stream = (struct oboe_aud_stream*)s;
    pj_status_t status;

    if (stream->running)
	return PJ_SUCCESS;

    if (stream->rec_engine) {
	status = stream->rec_engine->Start();
	if (status != PJ_SUCCESS)
	    goto on_error;
    }
    if (stream->play_engine) {
	status = stream->play_engine->Start();
	if (status != PJ_SUCCESS)
	    goto on_error;
    }
    
    stream->running = PJ_TRUE;
    PJ_LOG(4, (THIS_FILE, "Oboe stream started"));

    return PJ_SUCCESS;

on_error:
    if (stream->rec_engine)
	stream->rec_engine->Stop();
    if (stream->play_engine)
	stream->play_engine->Stop();

    PJ_LOG(4, (THIS_FILE, "Failed starting Oboe stream"));
    
    return status;
}

/* API: stop stream. */
static pj_status_t strm_stop(pjmedia_aud_stream *s)
{
    struct oboe_aud_stream *stream = (struct oboe_aud_stream*)s;

    if (!stream->running)
        return PJ_SUCCESS;

    stream->running = PJ_FALSE;

    if (stream->rec_engine)
	stream->rec_engine->Stop();
    if (stream->play_engine)
	stream->play_engine->Stop();

    PJ_LOG(4,(THIS_FILE, "Oboe stream stopped"));

    return PJ_SUCCESS;
}

/* API: destroy stream. */
static pj_status_t strm_destroy(pjmedia_aud_stream *s)
{
    struct oboe_aud_stream *stream = (struct oboe_aud_stream*)s;

    PJ_LOG(4,(THIS_FILE, "Destroying Oboe stream..."));

    /* Stop the stream */
    strm_stop(s);

    pj_pool_release(stream->pool);
    PJ_LOG(4, (THIS_FILE, "Oboe stream destroyed"));

    return PJ_SUCCESS;
}

#endif	/* PJMEDIA_AUDIO_DEV_HAS_OBOE */
