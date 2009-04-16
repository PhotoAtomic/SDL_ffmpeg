/*******************************************************************************
*                                                                              *
*   SDL_ffmpeg is a library for basic multimedia functionality.                *
*   SDL_ffmpeg is based on ffmpeg.                                             *
*                                                                              *
*   Copyright (C) 2007  Arjan Houben                                           *
*                                                                              *
*   SDL_ffmpeg is free software: you can redistribute it and/or modify         *
*   it under the terms of the GNU Lesser General Public License as published   *
*	by the Free Software Foundation, either version 3 of the License, or any   *
*   later version.                                                             *
*                                                                              *
*   This program is distributed in the hope that it will be useful,            *
*   but WITHOUT ANY WARRANTY; without even the implied warranty of             *
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the               *
*   GNU Lesser General Public License for more details.                        *
*                                                                              *
*   You should have received a copy of the GNU Lesser General Public License   *
*   along with this program.  If not, see <http://www.gnu.org/licenses/>.      *
*                                                                              *
*******************************************************************************/

/**
    @mainpage
    @version 1.0.0
    @author Arjan Houben

    SDL_ffmpeg is designed with ease of use in mind.
    Even the beginning programmer should be able to use this library
    so he or she can use multimedia in his/her program.
**/

#include <stdio.h>
#include <stdlib.h>

#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>

#include "libavformat/avformat.h"

#include "SDL/SDL_ffmpeg.h"

/**
\cond
*/

int __Y[256];
int __CrtoR[256];
int __CrtoG[256];
int __CbtoG[256];
int __CbtoB[256];

void initializeLookupTables() {

    float f;
    int i;

    for(i=0; i<256; i++) {

        f = (float)i;

		__Y[i] = (int)( 1.164 * (f-16.0) );

		__CrtoR[i] = (int)( 1.596 * (f-128.0) );

		__CrtoG[i] = (int)( 0.813 * (f-128.0) );
		__CbtoG[i] = (int)( 0.392 * (f-128.0) );

		__CbtoB[i] = (int)( 2.017 * (f-128.0) );
    }
}

int FFMPEG_init_was_called = 0;

int getPacket( SDL_ffmpegFile* );

SDL_ffmpegPacket* getAudioPacket( SDL_ffmpegFile* );

SDL_ffmpegPacket* getVideoPacket( SDL_ffmpegFile* );

int decodeAudioFrame( SDL_ffmpegFile*, AVPacket*, SDL_ffmpegAudioFrame* );

int decodeVideoFrame( SDL_ffmpegFile*, AVPacket*, SDL_ffmpegVideoFrame* );

void convertYUV420PtoRGBA( AVFrame *YUV420P, SDL_Surface *RGB444, int interlaced );

void convertYUV420PtoYUY2( AVFrame *YUV420P, SDL_Overlay *YUY2, int interlaced );

void convertRGBAtoYUV420P( const SDL_Surface *RGBA, AVFrame *YUV420P, int interlaced );

int SDL_ffmpegDecodeThread(void* data);

const SDL_ffmpegCodec SDL_ffmpegCodecPALDVD = {
    CODEC_ID_MPEG2VIDEO,
    720, 576,
    1, 25,
    6000000,
    -1, -1,
    CODEC_ID_MP2,
    2, 48000,
    192000,
    -1, -1
};

const SDL_ffmpegCodec SDL_ffmpegCodecPALDV = {
    CODEC_ID_DVVIDEO,
    720, 576,
    1, 25,
    6553600,
    -1, -1,
    CODEC_ID_DVAUDIO,
    2, 48000,
    256000,
    -1, -1
};

SDL_ffmpegFile* SDL_ffmpegCreateFile() {

    /* create SDL_ffmpegFile pointer */
    SDL_ffmpegFile *file = (SDL_ffmpegFile*)malloc( sizeof(SDL_ffmpegFile) );
    if(!file) return 0;

    memset( file, 0, sizeof(SDL_ffmpegFile) );

    file->streamMutex = SDL_CreateMutex();

    return file;
}

/**
\endcond
*/

/** \brief  Use this to free an SDL_ffmpegFile.

            This function stops the decoding thread if needed
            and flushes the buffers before releasing the memory.
            It also invalidates any frames aquired by
            SDL_ffmpegGetVideoFrame( SDL_ffmpegFile* ) or SDL_ffmpegGetAudioFrame( SDL_ffmpegFile* ).
\param      file SDL_ffmpegFile on which an action is required
*/
void SDL_ffmpegFree( SDL_ffmpegFile *file ) {

    SDL_ffmpegStream *s;

    if( !file ) return;

    SDL_ffmpegFlush(file);

    /* only write trailer when handling output streams */
    if( file->type == SDL_ffmpegOutputStream ) {
        av_write_trailer( file->_ffmpeg );
    }

    s = file->vs;
    while( s ) {

        SDL_ffmpegStream *old = s;

        s = s->next;

        SDL_DestroyMutex( old->mutex );

        av_free( old->decodeFrame );

        if( old->_ffmpeg ) avcodec_close( old->_ffmpeg->codec );

        free( old );
    }

    s = file->as;
    while( s ) {

        SDL_ffmpegStream *old = s;

        s = s->next;

        SDL_DestroyMutex( old->mutex );

        av_free( old->sampleBuffer );

        if( old->_ffmpeg ) avcodec_close( old->_ffmpeg->codec );

        free( old );
    }

    if( file->_ffmpeg ) {

        if( file->type == SDL_ffmpegInputStream ) {

            av_close_input_file( file->_ffmpeg );

        } else if(file->type == SDL_ffmpegOutputStream ) {

            url_fclose( file->_ffmpeg->pb );

            av_free( file->_ffmpeg );
        }
    }

    SDL_DestroyMutex( file->streamMutex );

    free(file);
}


/** \brief  Use this to free an SDL_ffmpegAudioFrame.

            This releases all buffers which where allocated in SDL_ffmpegCreateAudioFrame
\param      frame SDL_ffmpegAudioFrame which needs to be deleted
*/
void SDL_ffmpegFreeAudio(SDL_ffmpegAudioFrame* frame) {

    av_free( frame->buffer );

    free( frame );
}


/** \brief  Use this to free an SDL_ffmpegVideoFrame.

            This releases all buffers which where allocated in SDL_ffmpegCreateVideoFrame
\param      frame SDL_ffmpegVideoFrame which needs to be deleted
*/
void SDL_ffmpegFreeVideo(SDL_ffmpegVideoFrame* frame) {

    /* incomplete! */

    free( frame );
}


/** \brief  Use this to open the multimedia file of your choice.

            This function is used to open a multimedia file.

\param      filename string containing the location of the file
\returns    a pointer to a SDL_ffmpegFile structure, or NULL if a file could not be opened
*/
SDL_ffmpegFile* SDL_ffmpegOpen(const char* filename) {

    SDL_ffmpegFile *file;
    SDL_ffmpegStream **s;
    AVCodec *codec;

    /* register all codecs */
    if(!FFMPEG_init_was_called) {
        FFMPEG_init_was_called = 1;

        avcodec_register_all();
        av_register_all();
        initializeLookupTables();
    }

    /* open new ffmpegFile */
    file = SDL_ffmpegCreateFile();
    if(!file) return 0;

    /* information about format is stored in file->_ffmpeg */
    file->type = SDL_ffmpegInputStream;

    /* open the file */
    if(av_open_input_file( (AVFormatContext**)(&file->_ffmpeg), filename, 0, 0, 0) != 0) {
        fprintf(stderr, "could not open \"%s\"\n", filename);
        free(file);
        return 0;
    }

    /* retrieve format information */
    if(av_find_stream_info(file->_ffmpeg) < 0) {
        fprintf(stderr, "could not retrieve file info for \"%s\"\n", filename);
        free(file);
        return 0;
    }

    /* iterate through all the streams and store audio/video streams */
    for(int i=0; i<file->_ffmpeg->nb_streams; i++) {

        if( file->_ffmpeg->streams[i]->codec->codec_type == CODEC_TYPE_VIDEO ) {

            /* if this is a packet of the correct type we create a new stream */
            SDL_ffmpegStream* stream = (SDL_ffmpegStream*)malloc( sizeof(SDL_ffmpegStream) );

            if(stream) {

                /* we set our stream to zero */
                memset(stream, 0, sizeof(SDL_ffmpegStream));

                /* save unique streamid */
                stream->id = i;

                /* _ffmpeg holds data about streamcodec */
                stream->_ffmpeg = file->_ffmpeg->streams[i];

                /* get the correct decoder for this stream */
                codec = avcodec_find_decoder( stream->_ffmpeg->codec->codec_id );

                if(!codec) {
                    free(stream);
                    fprintf(stderr, "could not find codec\n");
                } else if(avcodec_open(file->_ffmpeg->streams[i]->codec, codec) < 0) {
                    free(stream);
                    fprintf(stderr, "could not open decoder\n");
                } else {

                    stream->mutex = SDL_CreateMutex();

                    stream->decodeFrame = avcodec_alloc_frame();

                    s = &file->vs;
                    while( *s ) {
                        *s = (*s)->next;
                    }

                    *s = stream;

                    file->videoStreams++;
                }
            }
        } else if( file->_ffmpeg->streams[i]->codec->codec_type == CODEC_TYPE_AUDIO ) {

            /* if this is a packet of the correct type we create a new stream */
            SDL_ffmpegStream* stream = (SDL_ffmpegStream*)malloc( sizeof(SDL_ffmpegStream) );

            if(stream) {
                /* we set our stream to zero */
                memset(stream, 0, sizeof(SDL_ffmpegStream));

                /* save unique streamid */
                stream->id = i;

                /* _ffmpeg holds data about streamcodec */
                stream->_ffmpeg = file->_ffmpeg->streams[i];

                /* get the correct decoder for this stream */
                codec = avcodec_find_decoder(file->_ffmpeg->streams[i]->codec->codec_id);

                if(!codec) {
                    free( stream );
                    fprintf(stderr, "could not find codec\n");
                } else if(avcodec_open(file->_ffmpeg->streams[i]->codec, codec) < 0) {
                    free( stream );
                    fprintf(stderr, "could not open decoder\n");
                } else {

                    stream->mutex = SDL_CreateMutex();

                    stream->sampleBuffer = av_malloc( AVCODEC_MAX_AUDIO_FRAME_SIZE * sizeof( int16_t ) );
                    stream->sampleBufferSize = 0;
                    stream->sampleBufferOffset = 0;
                    stream->sampleBufferTime = AV_NOPTS_VALUE;

                    s = &file->as;
                    while( *s ) {
                        *s = (*s)->next;
                    }

                    *s = stream;

                    file->audioStreams++;
                }
            }
        }
    }

    return file;
}


/** \brief  Use this to create the multimedia file of your choice.

            This function is used to create a multimedia file.

\param      filename string containing the location to which the data will be written
\returns    a pointer to a SDL_ffmpegFile structure, or NULL if a file could not be opened
*/
SDL_ffmpegFile* SDL_ffmpegCreate(const char* filename) {

    SDL_ffmpegFile *file;

    /* register all codecs */
    if(!FFMPEG_init_was_called) {
        FFMPEG_init_was_called = 1;

        avcodec_register_all();
        av_register_all();
        initializeLookupTables();
    }

    file = SDL_ffmpegCreateFile();

    file->_ffmpeg = avformat_alloc_context();

    /* guess output format based on filename */
    file->_ffmpeg->oformat = guess_format(0, filename, 0);

    if( !file->_ffmpeg->oformat ) {

        file->_ffmpeg->oformat = guess_format("dvd", 0, 0);
    }

    /* preload as shown in ffmpeg.c */
    file->_ffmpeg->preload = (int)(0.5*AV_TIME_BASE);

    /* max delay as shown in ffmpeg.c */
    file->_ffmpeg->max_delay = (int)(0.7*AV_TIME_BASE);

    /* open the output file, if needed */
    if( url_fopen( &file->_ffmpeg->pb, filename, URL_WRONLY ) < 0 ) {
        fprintf( stderr, "could not open '%s'\n", filename );
        SDL_ffmpegFree( file );
        return 0;
    }

    file->type = SDL_ffmpegOutputStream;

    return file;
}


/** \brief  Use this to add a SDL_ffmpegVideoFrame to file

            By adding frames to file, a video stream is build. If an audio stream
            is present, syncing of both streams needs to be done by user.
\param      file SDL_ffmpegFile to which a frame needs to be added
\returns    0 if frame was added, non-zero if an error occured
*/
int SDL_ffmpegAddVideoFrame( SDL_ffmpegFile *file, SDL_ffmpegVideoFrame *frame ) {

    /* when accesing audio/video stream, streamMutex should be locked */
    SDL_LockMutex( file->streamMutex );

    if( !file  || !file->videoStream || !frame || !frame->surface ) {
        SDL_UnlockMutex( file->streamMutex );
        return -1;
    }

    convertRGBAtoYUV420P( frame->surface, file->videoStream->encodeFrame, 0 );

    /* PAL = upper field first
    file->videoStream->encodeFrame->top_field_first = 1;
    */

    int out_size = avcodec_encode_video( file->videoStream->_ffmpeg->codec, file->videoStream->encodeFrameBuffer, file->videoStream->encodeFrameBufferSize, file->videoStream->encodeFrame );

    /* if zero size, it means the image was buffered */
    if( out_size > 0 ) {

        AVPacket pkt;
        av_init_packet( &pkt );

        /* set correct stream index for this packet */
        pkt.stream_index = file->videoStream->_ffmpeg->index;
        /* set keyframe flag if needed */
        if( file->videoStream->_ffmpeg->codec->coded_frame->key_frame ) pkt.flags |= PKT_FLAG_KEY;
        /* write encoded data into packet */
        pkt.data = file->videoStream->encodeFrameBuffer;
        /* set the correct size of this packet */
        pkt.size = out_size;
        /* set the correct duration of this packet */
        pkt.duration = AV_TIME_BASE / file->videoStream->_ffmpeg->time_base.den;

        /* if needed info is available, write pts for this packet */
        if (file->videoStream->_ffmpeg->codec->coded_frame->pts != AV_NOPTS_VALUE) {
            pkt.pts= av_rescale_q( file->videoStream->_ffmpeg->codec->coded_frame->pts, file->videoStream->_ffmpeg->codec->time_base, file->videoStream->_ffmpeg->time_base );
        }

        av_write_frame( file->_ffmpeg, &pkt );

        av_free_packet( &pkt );

        file->videoStream->frameCount++;
    }

    SDL_UnlockMutex( file->streamMutex );

    return 0;
}


/** \brief  Use this to add a SDL_ffmpegAudioFrame to file

            By adding frames to file, an audio stream is build. If a video stream
            is present, syncing of both streams needs to be done by user.
\param      file SDL_ffmpegFile to which a frame needs to be added
\returns    0 if frame was added, non-zero if an error occured
*/
int SDL_ffmpegAddAudioFrame( SDL_ffmpegFile *file, SDL_ffmpegAudioFrame *frame ) {

    /* when accesing audio/video stream, streamMutex should be locked */
    SDL_LockMutex( file->streamMutex );

    if( !file  || !file->audioStream || !frame ) {
        SDL_UnlockMutex( file->streamMutex );
        return -1;
    }

    AVPacket pkt;

    /* initialize a packet to write */
    av_init_packet( &pkt );

    /* set correct stream index for this packet */
    pkt.stream_index = file->audioStream->_ffmpeg->index;

    /* set keyframe flag if needed */
    pkt.flags |= PKT_FLAG_KEY;

    /* set the correct size of this packet */
    pkt.size = avcodec_encode_audio( file->audioStream->_ffmpeg->codec, (uint8_t*)file->audioStream->sampleBuffer, file->audioStream->sampleBufferSize, (int16_t*)frame->buffer );

    /* write encoded data into packet */
    pkt.data = (uint8_t*)file->audioStream->sampleBuffer;

    /* if needed info is available, write pts for this packet */
    if (file->audioStream->_ffmpeg->codec->coded_frame->pts != AV_NOPTS_VALUE) {
        pkt.pts= av_rescale_q( file->audioStream->_ffmpeg->codec->coded_frame->pts, file->audioStream->_ffmpeg->codec->time_base, file->audioStream->_ffmpeg->time_base );
    }

    /* write packet to stream */
    av_write_frame( file->_ffmpeg, &pkt );

    av_free_packet( &pkt );

    file->audioStream->frameCount++;

    SDL_UnlockMutex( file->streamMutex );

    return 0;
}

/** \brief  Use this to create a SDL_ffmpegAudioFrame

            With this frame, you can receive audio data from the stream using
            SDL_ffmpegGetAudioFrame.
\param      file SDL_ffmpegFile for which a frame needs to be created
\returns    Pointer to SDL_ffmpegAudioFrame, or NULL if no frame could be created
*/
SDL_ffmpegAudioFrame* SDL_ffmpegCreateAudioFrame( SDL_ffmpegFile *file, uint32_t bytes ) {

    /* when accesing audio/video stream, streamMutex should be locked */
    SDL_LockMutex( file->streamMutex );

    if( !file || !file->audioStream || ( !bytes && file->type == SDL_ffmpegInputStream ) ) {
        SDL_UnlockMutex( file->streamMutex );
        return 0;
    }

    /* allocate new frame */
    SDL_ffmpegAudioFrame *frame = (SDL_ffmpegAudioFrame*)malloc( sizeof( SDL_ffmpegAudioFrame ) );
    memset( frame, 0, sizeof( SDL_ffmpegAudioFrame ) );

    if( file->type == SDL_ffmpegOutputStream ) {
        bytes = file->audioStream->encodeAudioInputSize * 2 * file->audioStream->_ffmpeg->codec->channels;
    }

    SDL_UnlockMutex( file->streamMutex );

    /* set capacity of new frame */
    frame->capacity = bytes;

    /* allocate buffer */
    frame->buffer = av_malloc( bytes );

    /* initialize a non-valid timestamp */
    frame->pts = AV_NOPTS_VALUE;

    return frame;
}


/** \brief  Use this to create a SDL_ffmpegVideoFrame

            With this frame, you can receve video frames from the stream using
            SDL_ffmpegGetVideoFrame.
\param      file SDL_ffmpegFile for which a frame needs to be created
\param      format flag as used by SDL. The following options are implemented
            SDL_YUY2_OVERLAY. If you would like to receive a RGBA image, you can
            set both format and screen parameter to NULL.
\param      screen This is a pointer to the SDL_Surface as returned by SDL_SetVideoMode.
            This parameter is only required when YUV data is desired. When RGBA
            data is required, this parameter can be set to NULL.
\returns    Pointer to SDL_ffmpegVideoFrame, or NULL if no frame could be created
*/
SDL_ffmpegVideoFrame* SDL_ffmpegCreateVideoFrame( const SDL_ffmpegFile *file, const uint32_t format, SDL_Surface *screen ) {

    /* when accesing audio/video stream, streamMutex should be locked */
    SDL_LockMutex( file->streamMutex );

    if( !file || !file->videoStream ) {
        SDL_UnlockMutex( file->streamMutex );
        return 0;
    }

    SDL_ffmpegVideoFrame *frame = malloc( sizeof(SDL_ffmpegVideoFrame) );
    memset( frame, 0, sizeof(SDL_ffmpegVideoFrame) );

    if( format == SDL_YUY2_OVERLAY && screen ) {

        frame->overlay = SDL_CreateYUVOverlay( file->videoStream->_ffmpeg->codec->width, file->videoStream->_ffmpeg->codec->height, SDL_YUY2_OVERLAY, screen );
    }

    if( !format ) {

        frame->surface = SDL_CreateRGBSurface( 0, file->videoStream->_ffmpeg->codec->width, file->videoStream->_ffmpeg->codec->height, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000 );
    }

    SDL_UnlockMutex( file->streamMutex );

    return frame;
}


/** \brief  Use this to get new video data from file.

            Using this function, you can retreive video data from file. This data
            gets written to frame.
\param      file SDL_ffmpegFile from which the data is required
\param      frame SDL_ffmpegVideoFrame to which the data will be written
\returns    Pointer to SDL_ffmpegVideoFrame, or NULL if no frame was available.
*/
int SDL_ffmpegGetVideoFrame( SDL_ffmpegFile* file, SDL_ffmpegVideoFrame *frame ) {

    /* when accesing audio/video stream, streamMutex should be locked */
    SDL_LockMutex( file->streamMutex );

    if( !frame || !file || !file->videoStream ) {
        SDL_UnlockMutex( file->streamMutex );
        return 0;
    }

    SDL_LockMutex( file->videoStream->mutex );

    /* assume current frame is empty */
    frame->ready = 0;
    frame->last = 0;

    /* get new packet */
    SDL_ffmpegPacket *pack = getVideoPacket( file );

    while( !pack && !frame->last ) {

        pack = getVideoPacket( file );

        frame->last = getPacket( file );
    }

    while( pack && !frame->ready ) {

        /* when a frame is received, frame->ready will be set */
        decodeVideoFrame( file, pack->data, frame );

        /* destroy used packet */
        av_free_packet( pack->data );
        free( pack );

        pack = getVideoPacket( file );

        while( !pack && !frame->last ) {

            pack = getVideoPacket( file );

            frame->last = getPacket( file );
        }
    }

    /* pack retreived, but was not used, push it back in the buffer */
    if( pack ) {

        /* take current buffer as next pointer */
        pack->next = file->videoStream->buffer;

        /* store pack as current buffer */
        file->videoStream->buffer = pack;

    } else if( !frame->ready && frame->last ) {

        /* check if there is still a frame in the buffer */
        decodeVideoFrame( file, 0, frame );
    }

    SDL_UnlockMutex( file->videoStream->mutex );

    SDL_UnlockMutex( file->streamMutex );


    return frame->ready;
}


/** \brief  Get the desired audio stream from file.

            This returns a pointer to the requested stream. With this stream pointer you can
            get information about the stream, like language, samplerate, size etc.
            Based on this information you can choose the stream you want to use.
\param      file SDL_ffmpegFile from which the information is required
\param      audioID is the stream you whish to select.
\returns    Pointer to SDL_ffmpegStream, or NULL if selected stream could not be found
*/
SDL_ffmpegStream* SDL_ffmpegGetAudioStream(SDL_ffmpegFile *file, uint32_t audioID) {

    /* check if we have any audiostreams */
    if( !file || !file->audioStreams ) return 0;

    SDL_ffmpegStream *s = file->as;

    /* return audiostream linked to audioID */
    for(int i=0; i<audioID && s; i++) s = s->next;

    return s;
}


/** \brief  Select an audio stream from file.

            Use this function to select an audio stream for decoding.
            Using SDL_ffmpegGetAudioStream you can get information about the streams.
            Based on that you can chose the stream you want.
\param      file SDL_ffmpegFile on which an action is required
\param      audioID is the stream you whish to select. negative values de-select any audio stream.
\returns    -1 on error, otherwise 0
*/
int SDL_ffmpegSelectAudioStream(SDL_ffmpegFile* file, int audioID) {

    /* when changing audio/video stream, streamMutex should be locked */
    SDL_LockMutex( file->streamMutex );

    /* check if we have any audiostreams and if the requested ID is available */
    if( !file || !file->audioStreams || audioID >= file->audioStreams ) {
        SDL_UnlockMutex( file->streamMutex );
        return -1;
    }

    if( audioID < 0 ) {

        /* reset audiostream */
        file->audioStream = 0;

    } else {

        /* set current audiostream to stream linked to audioID */
        file->audioStream = file->as;

        for(int i=0; i<audioID && file->audioStream; i++) file->audioStream = file->audioStream->next;
    }

    SDL_UnlockMutex( file->streamMutex );

    return 0;
}


/** \brief  Get the desired video stream from file.

            This returns a pointer to the requested stream. With this stream pointer you can
            get information about the stream, like language, samplerate, size etc.
            Based on this information you can choose the stream you want to use.
\param      file SDL_ffmpegFile from which the information is required
\param      videoID is the stream you whish to select.
\returns    Pointer to SDL_ffmpegStream, or NULL if selected stream could not be found
*/
SDL_ffmpegStream* SDL_ffmpegGetVideoStream(SDL_ffmpegFile *file, uint32_t videoID) {

    /* check if we have any audiostreams */
    if( !file || !file->videoStreams ) return 0;

    /* check if the requested id is possible */
    if( videoID >= file->videoStreams ) return 0;

    SDL_ffmpegStream *s = file->vs;

    /* return audiostream linked to audioID */
    for(int i=0; i<videoID && s; i++) s = s->next;

    return s;
}


/** \brief  Select a video stream from file.

            Use this function to select a video stream for decoding.
            Using SDL_ffmpegGetVideoStream you can get information about the streams.
            Based on that you can chose the stream you want.
\param      file SDL_ffmpegFile on which an action is required
\param      videoID is the stream you whish to select.
\returns    -1 on error, otherwise 0
*/
int SDL_ffmpegSelectVideoStream(SDL_ffmpegFile* file, int videoID) {

    /* when changing audio/video stream, streamMutex should be locked */
    SDL_LockMutex( file->streamMutex );

    /* check if we have any videostreams */
    if( !file || !file->videoStreams ) {
        SDL_UnlockMutex( file->streamMutex );
        return -1;
    }

    /* check if the requested id is possible */
    if( videoID >= file->videoStreams ) {
        SDL_UnlockMutex( file->streamMutex );
        return -1;
    }

    if( videoID < 0 ) {

        /* reset videostream */
        file->videoStream = 0;

    } else {

        /* set current videostream to stream linked to videoID */
        file->videoStream = file->vs;

        /* keep searching for correct videostream */
        for(int i=0; i<videoID && file->videoStream; i++) file->videoStream = file->videoStream->next;

        /* check if pixel format is supported */
        if( file->videoStream->_ffmpeg->codec->pix_fmt != PIX_FMT_YUV420P/* &&
            file->videoStream->_ffmpeg->codec->pix_fmt != PIX_FMT_YUVJ420P*/ ) {
            printf( "unsupported pixel format [%i]\n", file->videoStream->_ffmpeg->codec->pix_fmt );
            file->videoStream = 0;
        }
    }

    SDL_UnlockMutex( file->streamMutex );

    return 0;
}

/** \brief  Seek to a certain point in file.

            Tries to seek to specified point in file.
\param      file SDL_ffmpegFile on which an action is required
\param      timestamp is represented in milliseconds.
\returns    -1 on error, otherwise 0
*/
int SDL_ffmpegSeek(SDL_ffmpegFile* file, uint64_t timestamp) {

    if( !file || SDL_ffmpegDuration( file ) < timestamp ) return -1;

    /* convert milliseconds to AV_TIME_BASE units */
    uint64_t seekPos = timestamp * (AV_TIME_BASE / 1000);

    /* AVSEEK_FLAG_BACKWARD means we jump to the first keyframe before seekPos */
    av_seek_frame( file->_ffmpeg, -1, seekPos, AVSEEK_FLAG_BACKWARD );

    /* set minimal timestamp to decode */
    file->minimalTimestamp = timestamp;

    /* flush buffers */
    SDL_ffmpegFlush( file );

    return 0;
}

/** \brief  Seek to a relative point in file.

            Tries to seek to new location, based on current location in file.
\param      file SDL_ffmpegFile on which an action is required
\param      timestamp is represented in milliseconds.
\returns    -1 on error, otherwise 0
*/
int SDL_ffmpegSeekRelative(SDL_ffmpegFile *file, int64_t timestamp) {

    /* same thing as normal seek, just take into account the current position */
    return SDL_ffmpegSeek(file, SDL_ffmpegGetPosition(file) + timestamp);
}

/**
\cond
*/
int SDL_ffmpegFlush(SDL_ffmpegFile *file) {

    /* check for file and permission to flush buffers */
    if( !file ) return -1;

    /* when accesing audio/video stream, streamMutex should be locked */
    SDL_LockMutex( file->streamMutex );

    /* if we have a valid audio stream, we flush it */
    if( file->audioStream ) {

        SDL_LockMutex( file->audioStream->mutex );

        SDL_ffmpegPacket *pack = file->audioStream->buffer;

        while( pack ) {

            SDL_ffmpegPacket *old = pack;

            pack = pack->next;

            av_free( old->data );
            free( old );
        }

        file->audioStream->buffer = 0;

        /* flush internal ffmpeg buffers */
        if( file->audioStream->_ffmpeg ) {

            avcodec_flush_buffers( file->audioStream->_ffmpeg->codec );
        }

        SDL_UnlockMutex( file->audioStream->mutex );
    }

    /* if we have a valid video stream, we flush some more */
    if( file->videoStream ) {

        SDL_LockMutex( file->videoStream->mutex );

        SDL_ffmpegPacket *pack = file->videoStream->buffer;

        while( pack ) {

            SDL_ffmpegPacket *old = pack;

            pack = pack->next;

            av_free( old->data );
            free( old );
        }

        file->videoStream->buffer = 0;

        /* flush internal ffmpeg buffers */
        if(file->videoStream->_ffmpeg) avcodec_flush_buffers( file->videoStream->_ffmpeg->codec );

        SDL_UnlockMutex( file->videoStream->mutex );
    }

    SDL_UnlockMutex( file->streamMutex );

    return 0;
}
/**
\endcond
*/


/** \brief  Use this to get a pointer to a SDL_ffmpegAudioFrame.

            If you receive a frame, it is valid until you receive a new frame, or
            until the file is freed, using SDL_ffmpegFree( SDL_ffmpegFile* ).
            I you use data from the frame, you should adjust the size member by
            the amount of data used in bytes. This is needed so that SDL_ffmpeg can
            calculate the next frame.
\param      file SDL_ffmpegFile from which the information is required
\returns    Pointer to SDL_ffmpegAudioFrame, or NULL if no frame was available.
*/
int SDL_ffmpegGetAudioFrame( SDL_ffmpegFile *file, SDL_ffmpegAudioFrame *frame ) {

    /* when accesing audio/video stream, streamMutex should be locked */
    SDL_LockMutex( file->streamMutex );

    if( !frame || !file || !file->audioStream ) {
        SDL_UnlockMutex( file->streamMutex );
        return 0;
    }

    /* lock audio buffer */
    SDL_LockMutex( file->audioStream->mutex );

    /* reset frame end pointer and size */
    frame->last = 0;
    frame->size = 0;

    /* get new packet */
    SDL_ffmpegPacket *pack = getAudioPacket( file );

    while( !pack && !frame->last ) {

        pack = getAudioPacket( file );

        frame->last = getPacket( file );
    }

    /* decodeAudioFrame will return true if data from pack was used
       frame will be updated with the new data */
    while( pack && decodeAudioFrame( file, pack->data, frame ) ) {

        /* destroy used packet */
        av_free_packet( pack->data );
        free( pack );
        pack = 0;

        /* check if new packet is required */
        if( frame->size < frame->capacity ) {

            /* try to get a new packet */
            pack = getAudioPacket( file );

            while( !pack && !frame->last ) {

                pack = getAudioPacket( file );

                frame->last = getPacket(file);
            }
        }
    }

    /* pack retreived, but was not used, push it back in the buffer */
    if( pack ) {

        /* take current buffer as next pointer */
        pack->next = file->audioStream->buffer;

        /* store pack as current buffer */
        file->audioStream->buffer = pack;
    }

    /* unlock audio buffer */
    SDL_UnlockMutex( file->audioStream->mutex );

    SDL_UnlockMutex( file->streamMutex );

    return ( frame->size == frame->capacity );
}


/** \brief  Returns the current position of the file in milliseconds.

\param      file SDL_ffmpegFile from which the information is required
\returns    -1 on error, otherwise the length of the file in milliseconds
*/
int64_t SDL_ffmpegGetPosition(SDL_ffmpegFile *file) {

    if( !file ) return -1;

    /* when accesing audio/video stream, streamMutex should be locked */
    SDL_LockMutex( file->streamMutex );

    int64_t pos = 0;

    if( file->audioStream ) {

        pos = file->audioStream->lastTimeStamp;
    }

    if( file->videoStream && file->videoStream->lastTimeStamp > pos ) {

        pos = file->videoStream->lastTimeStamp;
    }

    SDL_UnlockMutex( file->streamMutex );

    /* return the current playposition of our file */
    return pos;
}


/** \brief  This can be used to get a SDL_AudioSpec based on values found in file

            This returns a SDL_AudioSpec, if you have selected a valid audio
            stream. Otherwise, default values are used. 0 if valid audiostream was found, 1 there was no valid audio stream found.
\param      file SDL_ffmpegFile from which the information is required
\param      samples Amount of samples required every time the callback is called.
            Lower values mean less latency, but please note that SDL has a minimal value.
\param      callback Pointer to callback function
\returns    SDL_AudioSpec with values ready in according to the selected audio stream.
            If no valid audio stream was available, all values of returned SDL_AudioSpec are set to 0
*/
SDL_AudioSpec SDL_ffmpegGetAudioSpec(SDL_ffmpegFile *file, int samples, SDL_ffmpegCallback callback) {

    /* create audio spec */
    SDL_AudioSpec spec;

    memset(&spec, 0, sizeof(SDL_AudioSpec));

    /* when accesing audio/video stream, streamMutex should be locked */
    SDL_LockMutex( file->streamMutex );

    /* if we have a valid audiofile, we can use its data to create a
       more appropriate audio spec */
    if( file && file->audioStream ) {

        spec.format = AUDIO_S16SYS;
        spec.samples = samples;
        spec.userdata = file;
        spec.callback = callback;
        spec.freq = file->audioStream->_ffmpeg->codec->sample_rate;
        spec.channels = file->audioStream->_ffmpeg->codec->channels;
    }

    SDL_UnlockMutex( file->streamMutex );

    return spec;
}


/** \brief  Returns the Duration of the file in milliseconds.

            Please note that this value is guestimated by FFmpeg, it may differ from
            actual playing time.
\param      file SDL_ffmpegFile from which the information is required
\returns    -1 on error, otherwise the length of the file in milliseconds
*/
uint64_t SDL_ffmpegDuration(SDL_ffmpegFile *file) {

    if( !file ) return 0;

    if( file->type == SDL_ffmpegInputStream ) {
        /* returns the duration of the entire file, please note that ffmpeg doesn't
           always get this value right! so don't bet your life on it... */
        return file->_ffmpeg->duration / (AV_TIME_BASE / 1000);
    }

    if( file->type == SDL_ffmpegOutputStream ) {

        uint64_t v = SDL_ffmpegVideoDuration(file);
        uint64_t a = SDL_ffmpegAudioDuration(file);

        if( v > a ) return v;
        return a;
    }

    return 0;
}


/** \brief  Returns the duration of the audio stream in milliseconds.

            This value can be used to sync two output streams.
\param      file SDL_ffmpegFile from which the information is required
\returns    -1 on error, otherwise the length of the file in milliseconds
*/
uint64_t SDL_ffmpegAudioDuration(SDL_ffmpegFile *file) {

    if( !file || !file->audioStream ) return 0;

    if( file->type == SDL_ffmpegInputStream ) {

        return av_rescale( 1000 * file->audioStream->_ffmpeg->duration, file->audioStream->_ffmpeg->time_base.num, file->audioStream->_ffmpeg->time_base.den );
    }

    if( file->type == SDL_ffmpegOutputStream ) {

        return file->audioStream->frameCount * file->audioStream->encodeAudioInputSize / ( file->audioStream->_ffmpeg->codec->sample_rate / 1000 );

        return av_rescale( 1000 * file->audioStream->frameCount, file->audioStream->_ffmpeg->codec->time_base.num, file->audioStream->_ffmpeg->codec->time_base.den );
    }

    return 0;
}


/** \brief  Returns the duration of the video stream in milliseconds.

            This value can be used to sync two output streams.
\param      file SDL_ffmpegFile from which the information is required
\returns    -1 on error, otherwise the length of the file in milliseconds
*/
uint64_t SDL_ffmpegVideoDuration(SDL_ffmpegFile *file) {

    if( !file || !file->videoStream ) return 0;

    if( file->type == SDL_ffmpegInputStream ) {

        return av_rescale( 1000 * file->videoStream->_ffmpeg->duration, file->videoStream->_ffmpeg->time_base.num, file->videoStream->_ffmpeg->time_base.den );
    }

    if( file->type == SDL_ffmpegOutputStream ) {

        return av_rescale( 1000 * file->videoStream->frameCount, file->videoStream->_ffmpeg->codec->time_base.num, file->videoStream->_ffmpeg->codec->time_base.den );
    }

    return 0;
}

/** \brief  retreive the width/height of a frame beloning to file

            With this function you can get the width and height of a frame, belonging to
            your file. If there is no (valid) videostream selected w and h default
            to 320x240. Please not that you will have to make sure the pointers are
            allocated.

\param      file SDL_ffmpegFile from which the information is required
\param      w width
\param      h height
\returns    -1 on error, otherwise 0
*/
int SDL_ffmpegGetVideoSize(SDL_ffmpegFile *file, int *w, int *h) {

    if(!w || !h) return -1;

    /* if we have a valid video file selected, we use it
       if not, we send default values and return.
       by checking the return value you can check if you got a valid size */
    if( file->videoStream ) {
        *w = file->videoStream->_ffmpeg->codec->width;
        *h = file->videoStream->_ffmpeg->codec->height;
        return 0;
    }

    *w = 0;
    *h = 0;
    return -1;
}


/** \brief  This is used to check if a valid audio stream is selected.

\param      file SDL_ffmpegFile from which the information is required
\returns    1 if a valid video stream is selected, otherwise 0
*/
int SDL_ffmpegValidAudio(SDL_ffmpegFile* file) {

    /* this function is used to check if we selected a valid audio stream */
    return !!file->audioStream;
}


/** \brief  This is used to check if a valid video stream is selected.

\param      file SDL_ffmpegFile from which the information is required
\returns    1 if a valid video stream is selected, otherwise 0
*/
int SDL_ffmpegValidVideo(SDL_ffmpegFile* file) {

    /* this function is used to check if we selected a valid video stream */
    return !!file->videoStream;
}


/** \brief  This is used to add a video stream to file

\param      file SDL_ffmpegFile to which the stream will be added
\returns    The stream which was added, or NULL if no stream could be added.
*/
SDL_ffmpegStream* SDL_ffmpegAddVideoStream( SDL_ffmpegFile *file, SDL_ffmpegCodec codec ) {

    /* add a video stream */
    AVStream *stream = av_new_stream( file->_ffmpeg, 0 );
    if( !stream ) {
        fprintf( stderr, "could not alloc stream\n" );
        return 0;
    }

    stream->codec = avcodec_alloc_context();

    avcodec_get_context_defaults2( stream->codec, CODEC_TYPE_VIDEO );

    stream->codec->codec_id = codec.videoCodecID;
            /*file->_ffmpeg->oformat->video_codec;*/

    stream->codec->codec_type = CODEC_TYPE_VIDEO;

    stream->codec->bit_rate = codec.videoBitrate;
            /*1500000;*/

    /* resolution must be a multiple of two */
    stream->codec->width = codec.width;
    stream->codec->height = codec.height;

    /* set time_base */
    stream->codec->time_base.num = codec.framerateNum;
    stream->codec->time_base.den = codec.framerateDen;

    /* emit one intra frame every twelve frames at most */
    stream->codec->gop_size = 12;

    /* set pixel format */
    stream->codec->pix_fmt = PIX_FMT_YUV420P;

    /* set mpeg2 codec parameters */
    if( stream->codec->codec_id == CODEC_ID_MPEG2VIDEO ) {
        stream->codec->max_b_frames = 2;

//        stream->codec->flags |= CODEC_FLAG_CLOSED_GOP | CODEC_FLAG_INTERLACED_DCT;
//        stream->codec->flags2 |= CODEC_FLAG2_STRICT_GOP;
//        stream->codec->scenechange_threshold = 1000000000;
    }

    /* set mpeg1 codec parameters */
    if (stream->codec->codec_id == CODEC_ID_MPEG1VIDEO){
        /* needed to avoid using macroblocks in which some coeffs overflow
           this doesnt happen with normal video, it just happens here as the
           motion of the chroma plane doesnt match the luma plane */
        stream->codec->mb_decision = 2;
    }

    /* some formats want stream headers to be separate */
    if( file->_ffmpeg->oformat->flags & AVFMT_GLOBALHEADER ) {

        stream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
    }

    /* find the video encoder */
    AVCodec *videoCodec = avcodec_find_encoder( stream->codec->codec_id );
    if( !videoCodec ) {
        fprintf( stderr, "video codec not found\n" );
        return 0;
    }

    /* open the codec */
    if( avcodec_open( stream->codec, videoCodec ) < 0 ) {
        fprintf( stderr, "could not open video codec\n" );
        return 0;
    }

    /* create a new stream */
    SDL_ffmpegStream *str = (SDL_ffmpegStream*)malloc( sizeof(SDL_ffmpegStream) );

    if( str ) {

        /* we set our stream to zero */
        memset( str, 0, sizeof(SDL_ffmpegStream) );

        str->id = file->audioStreams + file->videoStreams;

        /* _ffmpeg holds data about streamcodec */
        str->_ffmpeg = stream;

        str->mutex = SDL_CreateMutex();

        str->encodeFrame = avcodec_alloc_frame();

        uint8_t *picture_buf;
        int size = avpicture_get_size( stream->codec->pix_fmt, stream->codec->width, stream->codec->height );
        picture_buf = (uint8_t*)av_malloc( size + FF_INPUT_BUFFER_PADDING_SIZE );
        avpicture_fill( (AVPicture*)str->encodeFrame, picture_buf, stream->codec->pix_fmt, stream->codec->width, stream->codec->height );

        str->encodeFrameBufferSize = stream->codec->width * stream->codec->height * 4 + FF_INPUT_BUFFER_PADDING_SIZE;

        str->encodeFrameBuffer = (uint8_t*)av_malloc( str->encodeFrameBufferSize );

        file->videoStreams++;

        /* find correct place to save the stream */
        SDL_ffmpegStream **s = &file->vs;
        while( *s ) {
            *s = (*s)->next;
        }

        *s = str;

        if( av_set_parameters( file->_ffmpeg, 0 ) < 0 ) {
            fprintf( stderr, "could not set encoding parameters\n" );
        }

        /* try to write a header */
        av_write_header( file->_ffmpeg );
    }

    return str;
}


/** \brief  This is used to add a video stream to file

\param      file SDL_ffmpegFile to which the stream will be added
\returns    The stream which was added, or NULL if no stream could be added.
*/
SDL_ffmpegStream* SDL_ffmpegAddAudioStream( SDL_ffmpegFile *file, SDL_ffmpegCodec codec ) {

    // add an audio stream
    AVStream *stream = av_new_stream( file->_ffmpeg, 1 );
    if( !stream ) {
        fprintf( stderr, "could not alloc stream\n");
        return 0;
    }

    stream->codec->codec_id = codec.audioCodecID;
            /*file->_ffmpeg->oformat->audio_codec;*/
    stream->codec->codec_type = CODEC_TYPE_AUDIO;
    stream->codec->bit_rate = codec.audioBitrate;
    stream->codec->sample_rate = codec.sampleRate;
    stream->codec->channels = codec.channels;

//    stream->codec->time_base.num = 1;
//    stream->codec->time_base.den = 41;

    // find the audio encoder
    AVCodec *audioCodec = avcodec_find_encoder( stream->codec->codec_id );
    if(!audioCodec) {
        fprintf( stderr, "audio codec not found\n" );
        return 0;
    }

    // open the codec
    if(avcodec_open( stream->codec, audioCodec ) < 0) {
        fprintf( stderr, "could not open audio codec\n" );
        return 0;
    }

    /* some formats want stream headers to be separate */
    if( file->_ffmpeg->oformat->flags & AVFMT_GLOBALHEADER ) {

        stream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
    }


    /* create a new stream */
    SDL_ffmpegStream *str = (SDL_ffmpegStream*)malloc( sizeof(SDL_ffmpegStream) );

    if( str ) {

        str->_ffmpeg = stream;

        /* we set our stream to zero */
        memset( str, 0, sizeof(SDL_ffmpegStream) );

        str->id = file->audioStreams + file->videoStreams;

        /* _ffmpeg holds data about streamcodec */
        str->_ffmpeg = stream;

        str->mutex = SDL_CreateMutex();

        str->sampleBufferSize = 10000;

        str->sampleBuffer = (int8_t*)av_malloc( str->sampleBufferSize );

        /* ugly hack for PCM codecs (will be removed ASAP with new PCM
           support to compute the input frame size in samples */
        if( stream->codec->frame_size <= 1 ) {

            str->encodeAudioInputSize = str->sampleBufferSize / stream->codec->channels;

            switch( stream->codec->codec_id ) {

                case CODEC_ID_PCM_S16LE:
                case CODEC_ID_PCM_S16BE:
                case CODEC_ID_PCM_U16LE:
                case CODEC_ID_PCM_U16BE:
                    str->encodeAudioInputSize >>= 1;
                    break;
                default:
                    break;
            }
        } else {
            str->encodeAudioInputSize = stream->codec->frame_size;
        }

        file->audioStreams++;

        /* find correct place to save the stream */
        SDL_ffmpegStream **s = &file->as;
        while( *s ) {
            *s = (*s)->next;
        }

        *s = str;

        if( av_set_parameters( file->_ffmpeg, 0 ) < 0 ) {
            fprintf( stderr, "could not set encoding parameters\n" );
            return 0;
        }

        /* try to write a header */
        av_write_header( file->_ffmpeg );
    }

    return str;
}

/**
\cond
*/

int getPacket( SDL_ffmpegFile *file ) {

    AVPacket *pack;
    int decode;

    /* create a packet for our data */
    pack = av_malloc( sizeof(AVPacket) );

    /* initialize packet */
    av_init_packet( pack );

    /* read a packet from the file */
    decode = av_read_frame( file->_ffmpeg, pack );

    /* if we did not get a packet, we probably reached the end of the file */
    if( decode < 0 ) {

        /* signal EOF */
        return 1;
    }

    /* we got a packet, lets handle it */

    /* try to allocate the packet */
    if( av_dup_packet( pack ) ) {

        /* error allocating packet */
        av_free_packet( pack );

    } else {

        /* If it's a packet from either of our streams, return it */
        if( file->audioStream && pack->stream_index == file->audioStream->id ) {

            /* prepare packet */
            SDL_ffmpegPacket *temp = (SDL_ffmpegPacket*)malloc( sizeof(SDL_ffmpegPacket) );
            temp->data = pack;
            temp->next = 0;

            SDL_ffmpegPacket **p = &file->audioStream->buffer;

            while( *p ) {
                p = &(*p)->next;
            }

            *p = temp;

        } else if( file->videoStream && pack->stream_index == file->videoStream->id ) {

            /* prepare packet */
            SDL_ffmpegPacket *temp = (SDL_ffmpegPacket*)malloc( sizeof(SDL_ffmpegPacket) );
            temp->data = pack;
            temp->next = 0;

//            SDL_LockMutex( file->videoStream->mutex );

            SDL_ffmpegPacket **p = &file->videoStream->buffer;

            while( *p ) {
                p = &(*p)->next;
            }

            *p = temp;

//            SDL_UnlockMutex( file->videoStream->mutex );

        } else {

            av_free_packet( pack );
        }
    }

    return 0;
}

SDL_ffmpegPacket* getAudioPacket( SDL_ffmpegFile *file ) {

    if( !file->audioStream ) return 0;

    /* file->audioStream->mutex should be locked before entering this function */

    SDL_ffmpegPacket *pack = 0;

    /* check if there are still packets in buffer */
    if( file->audioStream->buffer ) {

        pack = file->audioStream->buffer;

        file->audioStream->buffer = pack->next;
    }

    /* if a packet was found, return it */
    return pack;
}

SDL_ffmpegPacket* getVideoPacket( SDL_ffmpegFile *file ) {

    if( !file->videoStream ) return 0;

    /* file->videoStream->mutex should be locked before entering this function */

    SDL_ffmpegPacket *pack = 0;

    /* check if there are still packets in buffer */
    if( file->videoStream->buffer ) {

        pack = file->videoStream->buffer;

        file->videoStream->buffer = pack->next;
    }

    /* if a packet was found, return it */
    return pack;
}

int decodeAudioFrame( SDL_ffmpegFile *file, AVPacket *pack, SDL_ffmpegAudioFrame *frame ) {

    uint8_t *data;
    int size,
        len,
        audioSize;

    data = pack->data;
    size = pack->size;
    audioSize = AVCODEC_MAX_AUDIO_FRAME_SIZE * sizeof( int16_t );

    /* check if there is still data in the buffer */
    if( file->audioStream->sampleBufferSize ) {

        /* set new pts */
        if( !frame->size ) frame->pts = file->audioStream->sampleBufferTime;

        /* calculate free space in frame */
        int fs = frame->capacity - frame->size;

        /* check the amount of data which needs to be copied */
        if( fs < file->audioStream->sampleBufferSize ) {

            /* copy data from sampleBuffer into frame buffer until frame buffer is full */
            memcpy( frame->buffer+frame->size, file->audioStream->sampleBuffer+file->audioStream->sampleBufferOffset, fs );

            /* mark the amount of bytes still in the buffer */
            file->audioStream->sampleBufferSize -= fs;

            /* move offset accordingly */
            file->audioStream->sampleBufferOffset += fs;

            /* update framesize */
            frame->size = frame->capacity;

        } else {

            /* copy data from sampleBuffer into frame buffer until sampleBuffer is empty */
            memcpy( frame->buffer+frame->size, file->audioStream->sampleBuffer+file->audioStream->sampleBufferOffset, file->audioStream->sampleBufferSize );

            /* update framesize */
            frame->size += file->audioStream->sampleBufferSize;

            /* at this point, samplebuffer should have been handled */
            file->audioStream->sampleBufferSize = 0;

            /* no more data in buffer, reset offset */
            file->audioStream->sampleBufferOffset = 0;
        }

        /* return 0 to signal caller that 'pack' was not used */
        if( frame->size == frame->capacity ) return 0;
    }

    file->audioStream->_ffmpeg->codec->hurry_up = 0;

    /* calculate pts to determine wheter or not this frame should be stored */
    file->audioStream->sampleBufferTime = av_rescale( (pack->dts-file->audioStream->_ffmpeg->start_time)*1000, file->audioStream->_ffmpeg->time_base.num, file->audioStream->_ffmpeg->time_base.den );

    /* don't decode packets which are too old anyway */
    if( file->audioStream->sampleBufferTime != AV_NOPTS_VALUE && file->audioStream->sampleBufferTime < file->minimalTimestamp ) {

        file->audioStream->_ffmpeg->codec->hurry_up = 1;
    }

    while( size > 0 ) {

		/* Decode the packet */
		len = avcodec_decode_audio2( file->audioStream->_ffmpeg->codec, (int16_t*)file->audioStream->sampleBuffer, &audioSize, data, size );

		/* if an error occured, we skip the frame */
		if( len <= 0 || !audioSize ) {
            printf("error!\n");
            break;
        }

		/* change pointers */
		data += len;
		size -= len;
	}

	if( !file->audioStream->_ffmpeg->codec->hurry_up ) {

        /* set new pts */
        if( !frame->size ) frame->pts = file->audioStream->sampleBufferTime;

        /* room in frame */
        int fs = frame->capacity - frame->size;

        /* check if there is room at all */
        if( fs ) {

            /* check the amount of data which needs to be copied */
            if( fs < audioSize ) {

                /* copy data from sampleBuffer into frame buffer until frame buffer is full */
                memcpy( frame->buffer+frame->size, file->audioStream->sampleBuffer, fs );

                /* mark the amount of bytes still in the buffer */
                file->audioStream->sampleBufferSize = audioSize - fs;

                /* set the offset so the remaining data can be found */
                file->audioStream->sampleBufferOffset = fs;

                /* update framesize */
                frame->size = frame->capacity;

            } else {

                /* copy data from sampleBuffer into frame buffer until sampleBuffer is empty */
                memcpy( frame->buffer+frame->size, file->audioStream->sampleBuffer, audioSize );

                /* mark the amount of bytes still in the buffer */
                file->audioStream->sampleBufferSize = 0;

                /* reset buffer offset */
                file->audioStream->sampleBufferOffset = 0;

                /* update framesize */
                frame->size += audioSize;
            }

        } else {

            /* no room in frame, mark samplebuffer as full */
            file->audioStream->sampleBufferSize = audioSize;

            /* reset buffer offset */
            file->audioStream->sampleBufferOffset = 0;
        }
    }

    /* pack was used, return 1 */
    return 1;
}

int decodeVideoFrame( SDL_ffmpegFile* file, AVPacket *pack, SDL_ffmpegVideoFrame *frame ) {

    int got_frame = 0;

    if( pack ) {

        /* usefull when dealing with B frames */
        if( pack->dts == AV_NOPTS_VALUE ) {
            /* if we did not get a valid timestamp, we make one up based on the last
               valid timestamp + the duration of a frame */
            frame->pts = file->videoStream->lastTimeStamp + av_rescale(1000*pack->duration, file->videoStream->_ffmpeg->time_base.num, file->videoStream->_ffmpeg->time_base.den);
        } else {
            /* write timestamp into the buffer */
            frame->pts = av_rescale((pack->dts-file->videoStream->_ffmpeg->start_time)*1000, file->videoStream->_ffmpeg->time_base.num, file->videoStream->_ffmpeg->time_base.den);
        }

        /* check if we are decoding frames which we need not store */
        if( frame->pts != AV_NOPTS_VALUE && frame->pts < file->minimalTimestamp ) {

            file->videoStream->_ffmpeg->codec->hurry_up = 1;

        } else {

            file->videoStream->_ffmpeg->codec->hurry_up = 0;
        }

        /* Decode the packet */
        avcodec_decode_video( file->videoStream->_ffmpeg->codec, file->videoStream->decodeFrame, &got_frame, pack->data, pack->size);

    } else {

        /* check if there is still a frame left in the buffer */
        avcodec_decode_video( file->videoStream->_ffmpeg->codec, file->videoStream->decodeFrame, &got_frame, 0, 0 );
    }

    /* if we did not get a frame or we need to hurry, we return */
    if( got_frame && !file->videoStream->_ffmpeg->codec->hurry_up ) {

        #if 0
        /* if YUV data is scaled in the range of 8 - 235 instead of 0 - 255, we need to take this into account */
        if( file->videoStream->_ffmpeg->codec->pix_fmt == PIX_FMT_YUVJ420P ||
            file->videoStream->_ffmpeg->codec->pix_fmt == PIX_FMT_YUVJ422P ||
            file->videoStream->_ffmpeg->codec->pix_fmt == PIX_FMT_YUVJ444P ) scaled = 0;
        #endif

        /* convert YUV 420 to YUYV 422 data */
        if( frame->overlay && frame->overlay->format == SDL_YUY2_OVERLAY ) {

            convertYUV420PtoYUY2( file->videoStream->decodeFrame, frame->overlay, file->videoStream->decodeFrame->interlaced_frame );
        }

        /* convert YUV to RGB data */
        if( frame->surface ) {

			convertYUV420PtoRGBA( file->videoStream->decodeFrame, frame->surface, file->videoStream->decodeFrame->interlaced_frame );
        }

		/* we write the lastTimestamp we got */
		file->videoStream->lastTimeStamp = frame->pts;

		/* flag this frame as ready */
		frame->ready = 1;
	}

    return frame->ready;
}

inline int clamp0_255(int x) {
	x &= (~x) >> 31;
	x -= 255;
	x &= x >> 31;
	return x + 255;
}

void convertYUV420PtoRGBA( AVFrame *YUV420P, SDL_Surface *OUTPUT, int interlaced ) {

    uint8_t *Y, *U, *V;
	uint32_t *RGBA = OUTPUT->pixels;
    int x, y;

    for(y=0; y<OUTPUT->h; y++){

        Y = YUV420P->data[0] + YUV420P->linesize[0] * y;
        U = YUV420P->data[1] + YUV420P->linesize[1] * (y/2);
        V = YUV420P->data[2] + YUV420P->linesize[2] * (y/2);

		/* make sure we deinterlace before upsampling */
		if( interlaced ) {
            /* y & 3 means y % 3, but this should be faster */
			/* on scanline 2 and 3 we need to look at different lines */
            if( (y & 3) == 1 ) {
				U += YUV420P->linesize[1];
				V += YUV420P->linesize[2];
            } else if( (y & 3) == 2 ) {
				U -= YUV420P->linesize[1];
				V -= YUV420P->linesize[2];
			}
		}

        for(x=0; x<OUTPUT->w; x++){

			/* shift components to the correct place in pixel */
			*RGBA =   clamp0_255( __Y[*Y] + __CrtoR[*V] )							| /* red */
					( clamp0_255( __Y[*Y] - __CrtoG[*V] - __CbtoG[*U] )	<<  8 )		| /* green */
					( clamp0_255( __Y[*Y] + __CbtoB[*U] )				<< 16 )		| /* blue */
					0xFF000000;

			/* goto next pixel */
			RGBA++;

            /* full resolution luma, so we increment at every pixel */
            Y++;

			/* quarter resolution chroma, increment every other pixel */
            U += x&1;
			V += x&1;
        }
    }
}

void convertYUV420PtoYUY2scanline( const uint8_t *Y, const uint8_t *U, const uint8_t *V, uint32_t *YUVpacked, int w ) {

    /* devide width by 2 */
    w >>= 1;

    while( w-- ) {

        /* Y0 U0 Y1 V0 */
        *YUVpacked = (*Y) | ((*U) << 8) | ((*(++Y)) << 16) | ((*V) << 24);
        YUVpacked++;
        Y++;
        U++;
        V++;
    }
}

void convertYUV420PtoYUY2( AVFrame *YUV420P, SDL_Overlay *YUY2, int interlaced ) {

    const uint8_t   *Y = YUV420P->data[0],
                    *U = YUV420P->data[1],
                    *V = YUV420P->data[2];

    uint8_t *YUVpacked = YUY2->pixels[0];

    SDL_LockYUVOverlay( YUY2 );

    if( interlaced ) {

        /* handle 4 lines per loop */
        for(int y=0; y<(YUY2->h>>2); y++){

            /* line 0 */
            convertYUV420PtoYUY2scanline( Y, U, V, (uint32_t*)YUVpacked, YUY2->w );
            YUVpacked += YUY2->pitches[0];
            Y += YUV420P->linesize[0];
            U += YUV420P->linesize[1];
            V += YUV420P->linesize[2];

            /* line 1 */
            convertYUV420PtoYUY2scanline( Y, U, V, (uint32_t*)YUVpacked, YUY2->w );
            YUVpacked += YUY2->pitches[0];
            Y += YUV420P->linesize[0];
            U -= YUV420P->linesize[1];
            V -= YUV420P->linesize[2];

            /* line 2 */
            convertYUV420PtoYUY2scanline( Y, U, V, (uint32_t*)YUVpacked, YUY2->w );
            YUVpacked += YUY2->pitches[0];
            Y += YUV420P->linesize[0];
            U += YUV420P->linesize[1];
            V += YUV420P->linesize[2];

            /* line 3 */
            convertYUV420PtoYUY2scanline( Y, U, V, (uint32_t*)YUVpacked, YUY2->w );
            YUVpacked += YUY2->pitches[0];
            Y += YUV420P->linesize[0];
            U += YUV420P->linesize[1];
            V += YUV420P->linesize[2];
        }

    } else {

        /* handle 2 lines per loop */
        for(int y=0; y<(YUY2->h>>1); y++){

            /* line 0 */
            convertYUV420PtoYUY2scanline( Y, U, V, (uint32_t*)YUVpacked, YUY2->w );
            YUVpacked += YUY2->pitches[0];
            Y += YUV420P->linesize[0];
            U += YUV420P->linesize[1];
            V += YUV420P->linesize[2];

            /* line 1 */
            convertYUV420PtoYUY2scanline( Y, U, V, (uint32_t*)YUVpacked, YUY2->w );
            YUVpacked += YUY2->pitches[0];
            Y += YUV420P->linesize[0];
        }

    }

    SDL_UnlockYUVOverlay( YUY2 );
}

void convertRGBAtoYUV420Pscanline( uint8_t *Y, uint8_t *U, uint8_t *V, const uint32_t *RGBApacked, int w ) {

//    Y  =      (0.257 * R) + (0.504 * G) + (0.098 * B) + 16
//    Cr = V =  (0.439 * R) - (0.368 * G) - (0.071 * B) + 128
//    Cb = U = -(0.148 * R) - (0.291 * G) + (0.439 * B) + 128

    /* devide width by 2 */
    w >>= 1;

    while( w-- ) {

        *Y = (0.257 * ((*RGBApacked>>16)&0xFF)) + (0.504 * ((*RGBApacked>>8)&0xFF)) + (0.098 * (*RGBApacked&0xFF)) + 16;
        Y++;
        RGBApacked++;

        *U = -(0.148 * ((*RGBApacked>>16)&0xFF)) - (0.291 * ((*RGBApacked>>8)&0xFF)) + (0.439 * (*RGBApacked&0xFF)) + 128;
        U++;

        *V = (0.439 * ((*RGBApacked>>16)&0xFF)) - (0.368 * ((*RGBApacked>>8)&0xFF)) - (0.071 * (0.439 * (*RGBApacked&0xFF))) + 128;
        V++;

        *Y = (0.257 * ((*RGBApacked>>16)&0xFF)) + (0.504 * ((*RGBApacked>>8)&0xFF)) + (0.098 * (*RGBApacked&0xFF)) + 16;
        Y++;
        RGBApacked++;
    }
}

void convertRGBAtoYUV420P( const SDL_Surface *RGBA, AVFrame *YUV420P, int interlaced ) {

    int y;

    uint8_t   *Y = YUV420P->data[0],
              *U = YUV420P->data[1],
              *V = YUV420P->data[2];

    const uint32_t *RGBApacked = RGBA->pixels;

    if( interlaced ) {

        /* handle 4 lines per loop */
        for(y=0; y<(RGBA->h>>2); y++){

            /* line 0 */
            convertRGBAtoYUV420Pscanline( Y, U, V, RGBApacked, RGBA->w );
            RGBApacked += RGBA->w;
            Y += YUV420P->linesize[0];
            U += YUV420P->linesize[1];
            V += YUV420P->linesize[2];

            /* line 1 */
            convertRGBAtoYUV420Pscanline( Y, U, V, RGBApacked, RGBA->w );
            RGBApacked += RGBA->w;
            Y += YUV420P->linesize[0];
            U -= YUV420P->linesize[1];
            V -= YUV420P->linesize[2];

            /* line 2 */
            convertRGBAtoYUV420Pscanline( Y, U, V, RGBApacked, RGBA->w );
            RGBApacked += RGBA->w;
            Y += YUV420P->linesize[0];
            U += YUV420P->linesize[1];
            V += YUV420P->linesize[2];

            /* line 3 */
            convertRGBAtoYUV420Pscanline( Y, U, V, RGBApacked, RGBA->w );
            RGBApacked += RGBA->w;
            Y += YUV420P->linesize[0];
            U += YUV420P->linesize[1];
            V += YUV420P->linesize[2];
        }

    } else {

        /* handle 2 lines per loop */
        for(y=0; y<(RGBA->h>>1); y++){

            /* line 0 */
            convertRGBAtoYUV420Pscanline( Y, U, V, RGBApacked, RGBA->w );
            RGBApacked += RGBA->w;
            Y += YUV420P->linesize[0];
            U += YUV420P->linesize[1];
            V += YUV420P->linesize[2];

            /* line 1 */
            convertRGBAtoYUV420Pscanline( Y, U, V, RGBApacked, RGBA->w );
            RGBApacked += RGBA->w;
            Y += YUV420P->linesize[0];
        }

    }
}

/**
\endcond
*/