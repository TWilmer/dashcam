/*
Copyright (c) 2013, Broadcom Europe Ltd
Copyright (c) 2013, James Hughes
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/**
 * \file RaspiStill.c
 * Command line program to capture a still frame and encode it to file.
 * Also optionally display a preview/viewfinder of current camera input.
 *
 * \date 31 Jan 2013
 * \Author: James Hughes
 *
 * Description
 *
 * 3 components are created; camera, preview and JPG encoder.
 * Camera component has three ports, preview, video and stills.
 * This program connects preview and stills to the preview and jpg
 * encoder. Using mmal we don't need to worry about buffers between these
 * components, but we do need to handle buffers from the encoder, which
 * are simply written straight to the file in the requisite buffer callback.
 *
 * We use the RaspiCamControl code to handle the specific camera settings.
 */

// We use some GNU extensions (asprintf, basename)

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <memory.h>
#include <unistd.h>
#include <errno.h>
#include <sysexits.h>
#include <wiringPi.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


#define VERSION_STRING "v1.3.8"

#include "bcm_host.h"
#include "interface/vcos/vcos.h"

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_logging.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_connection.h"
#include "interface/mmal/mmal_parameters_camera.h"

#include "RaspiPreview.h"
#include "RaspiCamControl.h"
#include <semaphore.h>

// Standard port setting for the camera component
#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_CAPTURE_PORT 2

// Stills format information
// 0 implies variable
#define STILLS_FRAME_RATE_NUM 1
#define STILLS_FRAME_RATE_DEN 1

/// Video render needs at least 2 buffers.
#define VIDEO_OUTPUT_BUFFERS_NUM 3

#define MAX_USER_EXIF_TAGS 32
#define MAX_EXIF_PAYLOAD_LENGTH 128

/// Frame advance method
#define FRAME_NEXT_SINGLE 0
#define FRAME_NEXT_TIMELAPSE 1
#define FRAME_NEXT_KEYPRESS 2
#define FRAME_NEXT_FOREVER 3
#define FRAME_NEXT_GPIO 4
#define FRAME_NEXT_SIGNAL 5
#define FRAME_NEXT_IMMEDIATELY 6

static void signal_handler(int signal_number);
static void camera_opencv_callback(MMAL_PORT_T *port,
                                   MMAL_BUFFER_HEADER_T *buffer);

/** Structure containing all state information for the current run
 */
typedef struct {
  int timeout; /// Time taken before frame is grabbed and app then shuts down.
               /// Units are milliseconds
  int width;   /// Requested width of image
  int height;  /// requested height of image
  char camera_name[MMAL_PARAMETER_CAMERA_INFO_MAX_STR_LEN]; // Name of the
                                                            // camera sensor
  int quality; /// JPEG quality setting (1-100)
  int wantRAW; /// Flag for whether the JPEG metadata also contains the RAW
               /// bayer image
  const char *filename;   /// filename of output file
  char *linkname;         /// filename of output file
  int frameStart;         /// First number of frame output counter
  int verbose;            /// !0 if want detailed run information
  int demoMode;           /// Run app in demo mode
  int demoInterval;       /// Interval between camera settings changes
  MMAL_FOURCC_T encoding; /// Encoding to use for the output file.
  const char *exifTags[MAX_USER_EXIF_TAGS]; /// Array of pointers to tags
                                            /// supplied from the command line
  int numExifTags;                          /// Number of supplied tags
  int enableExifTags; /// Enable/Disable EXIF tags in output
  int timelapse; /// Delay between each picture in timelapse mode. If 0, disable
                 /// timelapse
  int fullResPreview;   /// If set, the camera preview port runs at capture
                        /// resolution. Reduces fps.
  int frameNextMethod;  /// Which method to use to advance to next frame
  int glCapture;        /// Save the GL frame-buffer instead of camera output
  int settings;         /// Request settings from the camera
  int cameraNum;        /// Camera number
  int burstCaptureMode; /// Enable burst mode
  int sensor_mode; /// Sensor mode. 0=auto. Check docs/forum for modes selected
                   /// by other values.
  int datetime;    /// Use DateTime instead of frame#
  int timestamp;   /// Use timestamp instead of frame#

  RASPIPREVIEW_PARAMETERS preview_parameters; /// Preview setup parameters

  MMAL_COMPONENT_T *camera_component;    /// Pointer to the camera component
  MMAL_COMPONENT_T *encoder_component;   /// Pointer to the encoder component
  MMAL_COMPONENT_T *null_sink_component; /// Pointer to the null sink component
  MMAL_CONNECTION_T *
      preview_connection; /// Pointer to the connection from camera to preview
  MMAL_CONNECTION_T *
      encoder_connection; /// Pointer to the connection from camera to encoder

  MMAL_POOL_T *encoder_pool; /// Pointer to the pool of buffers used by encoder
                             /// output port

} RASPISTILL_STATE;

/** Struct used to pass information in encoder port userdata to callback
 */
typedef struct {
  FILE *file_handle;                   /// File handle to write buffer data to.
  VCOS_SEMAPHORE_T complete_semaphore; /// semaphore which is posted when we
                                       /// reach end of frame (indicates end of
                                       /// capture or fault)
  RASPISTILL_STATE *
      pstate; /// pointer to our state in case required in callback
} PORT_USERDATA;

static void display_valid_parameters(char *app_name);
static void store_exif_tag(RASPISTILL_STATE *state, const char *exif_tag);

/// Comamnd ID's and Structure defining our command line options
#define CommandHelp 0
#define CommandWidth 1
#define CommandHeight 2
#define CommandQuality 3
#define CommandRaw 4
#define CommandOutput 5
#define CommandVerbose 6
#define CommandTimeout 7
#define CommandThumbnail 8
#define CommandDemoMode 9
#define CommandEncoding 10
#define CommandExifTag 11
#define CommandTimelapse 12
#define CommandFullResPreview 13
#define CommandLink 14
#define CommandKeypress 15
#define CommandSignal 16
#define CommandGL 17
#define CommandGLCapture 18
#define CommandSettings 19
#define CommandCamSelect 20
#define CommandBurstMode 21
#define CommandSensorMode 22
#define CommandDateTime 23
#define CommandTimeStamp 24
#define CommandFrameStart 25

static struct {
  const char *format;
  MMAL_FOURCC_T encoding;
} encoding_xref[] = {{"jpg", MMAL_ENCODING_JPEG},
                     {"bmp", MMAL_ENCODING_BMP},
                     {"gif", MMAL_ENCODING_GIF},
                     {"png", MMAL_ENCODING_PNG}};

static int encoding_xref_size =
    sizeof(encoding_xref) / sizeof(encoding_xref[0]);

static struct {
  const char *description;
  int nextFrameMethod;
} next_frame_description[] = {
    {"Single capture", FRAME_NEXT_SINGLE},
    {"Capture on timelapse", FRAME_NEXT_TIMELAPSE},
    {"Capture on keypress", FRAME_NEXT_KEYPRESS},
    {"Run forever", FRAME_NEXT_FOREVER},
    {"Capture on GPIO", FRAME_NEXT_GPIO},
    {"Capture on signal", FRAME_NEXT_SIGNAL},
};

RASPICAM_CAMERA_PARAMETERS CameraParameters;

static int next_frame_description_size =
    sizeof(next_frame_description) / sizeof(next_frame_description[0]);

static void set_sensor_defaults(RASPISTILL_STATE *state) {
  MMAL_COMPONENT_T *camera_info;
  MMAL_STATUS_T status;

  // Default to the OV5647 setup
  state->width = 1280;
  state->height = 720;
  strncpy(state->camera_name, "OV5647", MMAL_PARAMETER_CAMERA_INFO_MAX_STR_LEN);

  // Try to get the camera name and maximum supported resolution
  status =
      mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA_INFO, &camera_info);
  if (status == MMAL_SUCCESS) {
    MMAL_PARAMETER_CAMERA_INFO_T param;
    param.hdr.id = MMAL_PARAMETER_CAMERA_INFO;
    param.hdr.size =
        sizeof(param) - 4; // Deliberately undersize to check firmware veresion
    status = mmal_port_parameter_get(camera_info->control, &param.hdr);

    if (status != MMAL_SUCCESS) {
      // Running on newer firmware
      param.hdr.size = sizeof(param);
      status = mmal_port_parameter_get(camera_info->control, &param.hdr);
      if (status == MMAL_SUCCESS && param.num_cameras > 0) {
        // Take the parameters from the first camera listed.
        state->width = 1280;
        state->height = 720;
        strncpy(state->camera_name, param.cameras[0].camera_name,
                MMAL_PARAMETER_CAMERA_INFO_MAX_STR_LEN);
        state->camera_name[MMAL_PARAMETER_CAMERA_INFO_MAX_STR_LEN - 1] = 0;
      } else
        vcos_log_error(
            "Cannot read cameara info, keeping the defaults for OV5647");
    } else {
      // Older firmware
      // Nothing to do here, keep the defaults for OV5647
    }

    mmal_component_destroy(camera_info);
  } else {
    vcos_log_error("Failed to create camera_info component");
  }
}

/**
 * Assign a default set of parameters to the state passed in
 *
 * @param state Pointer to state structure to assign defaults to
 */
static void default_status(RASPISTILL_STATE *state) {
  if (!state) {
    vcos_assert(0);
    return;
  }

  state->timeout = 5000; // 5s delay before take image
  state->quality = 85;
  state->wantRAW = 0;
  state->filename = NULL;
  state->linkname = NULL;
  state->frameStart = 0;
  state->verbose = 1;
  state->demoMode = 0;
  state->demoInterval = 250; // ms
  state->camera_component = NULL;
  state->encoder_component = NULL;
  state->preview_connection = NULL;
  state->encoder_connection = NULL;
  state->encoder_pool = NULL;
  state->encoding = MMAL_ENCODING_JPEG;
  state->numExifTags = 0;
  state->enableExifTags = 1;
  state->timelapse = 0;
  state->fullResPreview = 0;
  state->frameNextMethod = FRAME_NEXT_SINGLE;
  state->glCapture = 0;
  state->settings = 0;
  state->cameraNum = 0;
  state->burstCaptureMode = 0;
  state->sensor_mode = 0;
  state->datetime = 0;
  state->timestamp = 0;

  // Setup for sensor specific parameters
  set_sensor_defaults(state);

  // Setup preview window defaults
  raspipreview_set_defaults(&state->preview_parameters);
}

/**
 * Dump image state parameters to stderr. Used for debugging
 *
 * @param state Pointer to state structure to assign defaults to
 */
static void dump_status(RASPISTILL_STATE *state) {
  int i;

  if (!state) {
    vcos_assert(0);
    return;
  }

  fprintf(stderr, "Width %d, Height %d, quality %d, filename %s\n",
          state->width, state->height, state->quality, state->filename);
  fprintf(stderr, "Time delay %d, Raw %s\n", state->timeout,
          state->wantRAW ? "yes" : "no");
  fprintf(stderr, "Link to latest frame enabled ");
  if (state->linkname) {
    fprintf(stderr, " yes, -> %s\n", state->linkname);
  } else {
    fprintf(stderr, " no\n");
  }
  fprintf(stderr, "Full resolution preview %s\n",
          state->fullResPreview ? "Yes" : "No");

  fprintf(stderr, "Capture method : ");
  for (i = 0; i < next_frame_description_size; i++) {
    if (state->frameNextMethod == next_frame_description[i].nextFrameMethod)
      fprintf(stderr, "%s", next_frame_description[i].description);
  }
  fprintf(stderr, "\n\n");

  if (state->enableExifTags) {
    if (state->numExifTags) {
      fprintf(stderr, "User supplied EXIF tags :\n");

      for (i = 0; i < state->numExifTags; i++) {
        fprintf(stderr, "%s", state->exifTags[i]);
        if (i != state->numExifTags - 1)
          fprintf(stderr, ",");
      }
      fprintf(stderr, "\n\n");
    }
  } else
    fprintf(stderr, "EXIF tags disabled\n");

  raspipreview_dump_parameters(&state->preview_parameters);
}

/**
 * Display usage information for the application to stdout
 *
 * @param app_name String to display as the application name
 */
static void display_valid_parameters(char *app_name) {
  fprintf(stdout, "Runs camera for specific time, and take JPG capture at end "
                  "if requested\n\n");
  fprintf(stdout, "usage: %s [options]\n\n", app_name);

  fprintf(stdout, "Image parameter commands\n\n");

  // Help for preview options
  raspipreview_display_help();

  // Now display any help information from the camcontrol code

  fprintf(stdout, "\n");

  return;
}

/**
 *  buffer header callback function for camera control
 *
 *  No actions taken in current version
 *
 * @param port Pointer to port from which callback originated
 * @param buffer mmal buffer header pointer
 */
static void camera_control_callback(MMAL_PORT_T *port,
                                    MMAL_BUFFER_HEADER_T *buffer) {
  //printf("Control Callback called\n");
  if (buffer->cmd == MMAL_EVENT_PARAMETER_CHANGED) {
    MMAL_EVENT_PARAMETER_CHANGED_T *param =
        (MMAL_EVENT_PARAMETER_CHANGED_T *)buffer->data;
    switch (param->hdr.id) {
    case MMAL_PARAMETER_CAMERA_SETTINGS: {
      MMAL_PARAMETER_CAMERA_SETTINGS_T *settings =
          (MMAL_PARAMETER_CAMERA_SETTINGS_T *)param;
      vcos_log_error("Exposure now %u, analog gain %u/%u, digital gain %u/%u",
                     settings->exposure, settings->analog_gain.num,
                     settings->analog_gain.den, settings->digital_gain.num,
                     settings->digital_gain.den);
      vcos_log_error("AWB R=%u/%u, B=%u/%u", settings->awb_red_gain.num,
                     settings->awb_red_gain.den, settings->awb_blue_gain.num,
                     settings->awb_blue_gain.den);
    } break;
    }
  } else if (buffer->cmd == MMAL_EVENT_ERROR) {
    vcos_log_error("No data received from sensor. Check all connections, "
                   "including the Sunny one on the camera board");
  } else
    vcos_log_error("Received unexpected camera control callback event, 0x%08x",
                   buffer->cmd);

  mmal_buffer_header_release(buffer);
}

static void camera_opencv_callback(MMAL_PORT_T *port,
                                   MMAL_BUFFER_HEADER_T *buffer) {
  //printf("OpenCV Callback called\n");

  if (buffer->flags & (MMAL_BUFFER_HEADER_FLAG_FRAME_END |
                       MMAL_BUFFER_HEADER_FLAG_TRANSMISSION_FAILED)) {
    //	 printf("Start it\n");
  }

  // release buffer back to the pool
  mmal_buffer_header_release(buffer);

  // and send one back to the port (if still open)
  if (port->is_enabled) {
    MMAL_STATUS_T status = MMAL_SUCCESS;
    MMAL_BUFFER_HEADER_T *new_buffer;

    new_buffer = mmal_queue_get((MMAL_QUEUE_T *)port->userdata);

    if (new_buffer) {
      status = mmal_port_send_buffer(port, new_buffer);
    }
    if (!new_buffer || status != MMAL_SUCCESS)
      vcos_log_error("Unable to return a buffer to the encoder port");
  }
}

int outputFileFD=0;
/**
 *  buffer header callback function for encoder
 *
 *  Callback will dump buffer data to the specific file
 *
 * @param port Pointer to port from which callback originated
 * @param buffer mmal buffer header pointer
 */
static void encoder_buffer_callback(MMAL_PORT_T *port,
                                    MMAL_BUFFER_HEADER_T *buffer) {
  int complete = 0;

  // We pass our file handle and other stuff in via the userdata field.

  PORT_USERDATA *pData = (PORT_USERDATA *)port->userdata;

  if (pData) {
    if(outputFileFD==-1){
       outputFileFD=open("/var/www/html/left.jpg",  O_RDWR | O_CREAT);
       fchmod(outputFileFD, S_IROTH);
    }

      


    if (buffer->length && outputFileFD>0) {
      mmal_buffer_header_mem_lock(buffer);
      int lenWritten = write(outputFileFD, buffer->data,  buffer->length);
      if(lenWritten!= buffer->length)
      {
        complete=1;
         printf("Write error, aborting\n");
      }

      mmal_buffer_header_mem_unlock(buffer);
    }


    // Now flag if we have completed
    if (buffer->flags & (MMAL_BUFFER_HEADER_FLAG_FRAME_END |
                         MMAL_BUFFER_HEADER_FLAG_TRANSMISSION_FAILED))
      complete = 1;
  } else {
    vcos_log_error("Received a encoder buffer callback with no state");
  }

  // release buffer back to the pool
  mmal_buffer_header_release(buffer);

  // and send one back to the port (if still open)
  if (port->is_enabled) {
    MMAL_STATUS_T status = MMAL_SUCCESS;
    MMAL_BUFFER_HEADER_T *new_buffer;

    new_buffer = mmal_queue_get(pData->pstate->encoder_pool->queue);

    if (new_buffer) {
      status = mmal_port_send_buffer(port, new_buffer);
    }
    if (!new_buffer || status != MMAL_SUCCESS)
      vcos_log_error("Unable to return a buffer to the encoder port");
  }

  if (complete) {
    vcos_semaphore_post(&(pData->complete_semaphore));
    close(outputFileFD);
    outputFileFD=0;
  }
}

/**
 * Create the camera component, set up its ports
 *
 * @param state Pointer to state control struct. camera_component member set to
 *the created camera_component if successful.
 *
 * @return MMAL_SUCCESS if all OK, something else otherwise
 *
 */
static MMAL_STATUS_T create_camera_component(RASPISTILL_STATE *state) {
  MMAL_COMPONENT_T *camera = 0;
  MMAL_ES_FORMAT_T *format;
  MMAL_PORT_T *preview_port = NULL, *video_port = NULL, *still_port = NULL;
  MMAL_STATUS_T status;
  MMAL_POOL_T *pool = 0;
  int num = 0;
  int q = 0;
  ;
  MMAL_PARAMETER_INT32_T camera_num = {
      {MMAL_PARAMETER_CAMERA_NUM, sizeof(camera_num)}, state->cameraNum};

  /* Create the component */
  status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);

  if (status != MMAL_SUCCESS) {
    vcos_log_error("Failed to create camera component");
    goto error;
  }

  if (status != MMAL_SUCCESS) {
    vcos_log_error("Could not set stereo mode : error %d", status);
    goto error;
  }

  status = mmal_port_parameter_set(camera->control, &camera_num.hdr);

  if (status != MMAL_SUCCESS) {
    vcos_log_error("Could not select camera : error %d", status);
    goto error;
  }

  if (!camera->output_num) {
    status = MMAL_ENOSYS;
    vcos_log_error("Camera doesn't have output ports");
    goto error;
  }

  status = mmal_port_parameter_set_uint32(
      camera->control, MMAL_PARAMETER_CAMERA_CUSTOM_SENSOR_CONFIG,
      state->sensor_mode);

  if (status != MMAL_SUCCESS) {
    vcos_log_error("Could not set sensor mode : error %d", status);
    goto error;
  }

  preview_port = camera->output[MMAL_CAMERA_PREVIEW_PORT];
  video_port = camera->output[MMAL_CAMERA_VIDEO_PORT];
  still_port = camera->output[MMAL_CAMERA_CAPTURE_PORT];

  if (state->settings) {
    MMAL_PARAMETER_CHANGE_EVENT_REQUEST_T change_event_request = {
        {MMAL_PARAMETER_CHANGE_EVENT_REQUEST,
         sizeof(MMAL_PARAMETER_CHANGE_EVENT_REQUEST_T)},
        MMAL_PARAMETER_CAMERA_SETTINGS,
        1};

    status =
        mmal_port_parameter_set(camera->control, &change_event_request.hdr);
    if (status != MMAL_SUCCESS) {
      vcos_log_error("No camera settings events");
    }
  }

  // Enable the camera, and tell it its control callback function
  status = mmal_port_enable(camera->control, camera_control_callback);

  if (status != MMAL_SUCCESS) {
    vcos_log_error("Unable to enable control port : error %d", status);
    goto error;
  }

  //  set up the camera configuration
  {
    MMAL_PARAMETER_CAMERA_CONFIG_T cam_config = {
        {MMAL_PARAMETER_CAMERA_CONFIG, sizeof(cam_config)},
        .max_stills_w = state->width,
        .max_stills_h = state->height,
        .stills_yuv422 = 0,
        .one_shot_stills = 1,
        .max_preview_video_w = state->preview_parameters.previewWindow.width,
        .max_preview_video_h = state->preview_parameters.previewWindow.height,
        .num_preview_video_frames = 3,
        .stills_capture_circular_buffer_height = 0,
        .fast_preview_resume = 0,
        .use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RESET_STC};

    if (state->fullResPreview) {
      cam_config.max_preview_video_w = state->width;
      cam_config.max_preview_video_h = state->height;
    }

    mmal_port_parameter_set(camera->control, &cam_config.hdr);
  }

  // Now set up the port formats

  format = preview_port->format;
  format->encoding = MMAL_ENCODING_OPAQUE;
  format->encoding_variant = MMAL_ENCODING_I420;

  if (state->fullResPreview) {
    // In this mode we are forcing the preview to be generated from the full
    // capture resolution.
    // This runs at a max of 15fps with the OV5647 sensor.
    format->es->video.width = VCOS_ALIGN_UP(state->width, 32);
    format->es->video.height = VCOS_ALIGN_UP(state->height, 16);
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = state->width;
    format->es->video.crop.height = state->height;
    format->es->video.frame_rate.num = FULL_RES_PREVIEW_FRAME_RATE_NUM;
    format->es->video.frame_rate.den = FULL_RES_PREVIEW_FRAME_RATE_DEN;
  } else {
    // Use a full FOV 4:3 mode
    format->es->video.width =
        VCOS_ALIGN_UP(state->preview_parameters.previewWindow.width, 32);
    format->es->video.height =
        VCOS_ALIGN_UP(state->preview_parameters.previewWindow.height, 16);
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width =
        state->preview_parameters.previewWindow.width;
    format->es->video.crop.height =
        state->preview_parameters.previewWindow.height;
    format->es->video.frame_rate.num = PREVIEW_FRAME_RATE_NUM;
    format->es->video.frame_rate.den = PREVIEW_FRAME_RATE_DEN;
  }

  status = mmal_port_format_commit(preview_port);
  if (status != MMAL_SUCCESS) {
    vcos_log_error("camera viewfinder format couldn't be set");
    goto error;
  }

  // Set the same format on the video  port (which we don't use here)

  mmal_format_full_copy(video_port->format, format);
  format = video_port->format;
  format->encoding = MMAL_ENCODING_I420;
  format->encoding_variant = MMAL_ENCODING_I420;
  format->es->video.frame_rate.num = 30;
  format->es->video.frame_rate.den = 1;
  video_port->buffer_num = 4;
  video_port->buffer_size =
      format->es->video.width * format->es->video.height * 3 / 2;
  /*format=video_port->format;
    format->encoding = MMAL_ENCODING_I420;
    format->encoding_variant = MMAL_ENCODING_I420;
format->es->video.width = VCOS_ALIGN_UP(state->width, 32);
 format->es->video.height = VCOS_ALIGN_UP(state->height, 16);
 format->es->video.crop.x = 0;
 format->es->video.crop.y = 0;
 format->es->video.crop.width = state->width;
 format->es->video.crop.height = state->height;
 video_port->buffer_num = 3;
 video_port->buffer_size = format->es->video.width * format->es->video.height  *
3 / 2;
    format->es->video.frame_rate.num = PREVIEW_FRAME_RATE_NUM;
    format->es->video.frame_rate.den = PREVIEW_FRAME_RATE_DEN;
*/

  status = mmal_port_format_commit(video_port);

  if (status != MMAL_SUCCESS) {
    vcos_log_error("camera video format couldn't be set");
    goto error;
  }

  // Ensure there are enough buffers to avoid dropping frames
  if (video_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
    video_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;

  format = still_port->format;

  // Set our stills format on the stills (for encoder) port
  format->encoding = MMAL_ENCODING_OPAQUE;
  format->es->video.width = VCOS_ALIGN_UP(state->width, 32);
  format->es->video.height = VCOS_ALIGN_UP(state->height, 16);
  format->es->video.crop.x = 0;
  format->es->video.crop.y = 0;
  format->es->video.crop.width = state->width;
  format->es->video.crop.height = state->height;
  format->es->video.frame_rate.num = STILLS_FRAME_RATE_NUM;
  format->es->video.frame_rate.den = STILLS_FRAME_RATE_DEN;

  printf("Video foram %d x %d \n", format->es->video.width,
         format->es->video.height);

  status = mmal_port_format_commit(still_port);

  if (status != MMAL_SUCCESS) {
    vcos_log_error("camera still format couldn't be set");
    goto error;
  }

  /* Ensure there are enough buffers to avoid dropping frames */
  if (still_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
    still_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;

  state->camera_component = camera;
  printf("Create opencv pool with %d buffer of size %d\n",
         video_port->buffer_num, video_port->buffer_size);
  pool = mmal_port_pool_create(video_port, video_port->buffer_num,
                               video_port->buffer_size);

  if (!pool) {
    vcos_log_error(
        "Failed to create buffer header pool for encoder output port %s",
        video_port->name);
  }

  /* Enable component */
  status = mmal_component_enable(camera);

  if (status != MMAL_SUCCESS) {
    vcos_log_error("camera component couldn't be enabled");
    goto error;
  }

  if (state->verbose)
    fprintf(stderr, "Enable camera video port to opencv.\n");

  status = mmal_port_enable(video_port, camera_opencv_callback);
  if (status != MMAL_SUCCESS) {
    vcos_log_error("camera component couldn't enable opencv");
    goto error;
  }
  /* Create pool of buffer headers for the output port to consume */
  // Send all the buffers to the camera output port
  num = mmal_queue_length(pool->queue);
  printf("opencv queue length %d\n", num);

  for (q = 0; q < num; q++) {
    MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(pool->queue);

    if (!buffer)
      vcos_log_error("Unable to get a required buffer %d from pool queue", q);

    if (mmal_port_send_buffer(video_port, buffer) != MMAL_SUCCESS)
      vcos_log_error("Unable to send a buffer to camera output port (%d)", q);
    printf("Sent buffer %d to video port\n", q);
  }
  raspicamcontrol_set_defaults(&CameraParameters);
  raspicamcontrol_set_all_parameters(camera, &CameraParameters);
  video_port->userdata = (MMAL_PORT_USERDATA_T *)pool->queue;

  if (state->verbose)
    fprintf(stderr, "Camera component done\n");



   mmal_port_parameter_set_int32(camera->output[0], MMAL_PARAMETER_ROTATION, 180);
   mmal_port_parameter_set_int32(camera->output[1], MMAL_PARAMETER_ROTATION, 180);
   mmal_port_parameter_set_int32(camera->output[2], MMAL_PARAMETER_ROTATION, 180);


  return status;

error:

  if (camera)
    mmal_component_destroy(camera);

  return status;
}

/**
 * Destroy the camera component
 *
 * @param state Pointer to state control struct
 *
 */
static void destroy_camera_component(RASPISTILL_STATE *state) {
  if (state->camera_component) {
    mmal_component_destroy(state->camera_component);
    state->camera_component = NULL;
  }
}

/**
 * Create the encoder component, set up its ports
 *
 * @param state Pointer to state control struct. encoder_component member set to
 *the created camera_component if successful.
 *
 * @return a MMAL_STATUS, MMAL_SUCCESS if all OK, something else otherwise
 */
static MMAL_STATUS_T create_encoder_component(RASPISTILL_STATE *state) {
  MMAL_COMPONENT_T *encoder = 0;
  MMAL_PORT_T *encoder_input = NULL, *encoder_output = NULL;
  MMAL_STATUS_T status;
  MMAL_POOL_T *pool;

  status =
      mmal_component_create(MMAL_COMPONENT_DEFAULT_IMAGE_ENCODER, &encoder);

  if (status != MMAL_SUCCESS) {
    vcos_log_error("Unable to create JPEG encoder component");
    goto error;
  }

  if (!encoder->input_num || !encoder->output_num) {
    status = MMAL_ENOSYS;
    vcos_log_error("JPEG encoder doesn't have input/output ports");
    goto error;
  }

  encoder_input = encoder->input[0];
  encoder_output = encoder->output[0];

  // We want same format on input and output
  mmal_format_copy(encoder_output->format, encoder_input->format);

  // Specify out output format
  encoder_output->format->encoding = state->encoding;

  encoder_output->buffer_size = encoder_output->buffer_size_recommended;

  if (encoder_output->buffer_size < encoder_output->buffer_size_min)
    encoder_output->buffer_size = encoder_output->buffer_size_min;

  encoder_output->buffer_num = encoder_output->buffer_num_recommended;

  if (encoder_output->buffer_num < encoder_output->buffer_num_min)
    encoder_output->buffer_num = encoder_output->buffer_num_min;

  // Commit the port changes to the output port
  status = mmal_port_format_commit(encoder_output);

  if (status != MMAL_SUCCESS) {
    vcos_log_error("Unable to set format on video encoder output port");
    goto error;
  }

  // Set the JPEG quality level
  status = mmal_port_parameter_set_uint32(
      encoder_output, MMAL_PARAMETER_JPEG_Q_FACTOR, state->quality);

  if (status != MMAL_SUCCESS) {
    vcos_log_error("Unable to set JPEG quality");
    goto error;
  }

  //  Enable component
  status = mmal_component_enable(encoder);

  if (status != MMAL_SUCCESS) {
    vcos_log_error("Unable to enable video encoder component");
    goto error;
  }

  /* Create pool of buffer headers for the output port to consume */
  pool = mmal_port_pool_create(encoder_output, encoder_output->buffer_num,
                               encoder_output->buffer_size);

  if (!pool) {
    vcos_log_error(
        "Failed to create buffer header pool for encoder output port %s",
        encoder_output->name);
  }

  state->encoder_pool = pool;
  state->encoder_component = encoder;

  if (state->verbose)
    fprintf(stderr, "Encoder component done\n");

  return status;

error:

  if (encoder)
    mmal_component_destroy(encoder);

  return status;
}

/**
 * Destroy the encoder component
 *
 * @param state Pointer to state control struct
 *
 */
static void destroy_encoder_component(RASPISTILL_STATE *state) {
  // Get rid of any port buffers first
  if (state->encoder_pool) {
    mmal_port_pool_destroy(state->encoder_component->output[0],
                           state->encoder_pool);
  }

  if (state->encoder_component) {
    mmal_component_destroy(state->encoder_component);
    state->encoder_component = NULL;
  }
}

/**
 * Add an exif tag to the capture
 *
 * @param state Pointer to state control struct
 * @param exif_tag String containing a "key=value" pair.
 * @return  Returns a MMAL_STATUS_T giving result of operation
 */
static MMAL_STATUS_T add_exif_tag(RASPISTILL_STATE *state,
                                  const char *exif_tag) {
  MMAL_STATUS_T status;
  MMAL_PARAMETER_EXIF_T *exif_param = (MMAL_PARAMETER_EXIF_T *)calloc(
      sizeof(MMAL_PARAMETER_EXIF_T) + MAX_EXIF_PAYLOAD_LENGTH, 1);

  vcos_assert(state);
  vcos_assert(state->encoder_component);

  // Check to see if the tag is present or is indeed a key=value pair.
  if (!exif_tag || strchr(exif_tag, '=') == NULL ||
      strlen(exif_tag) > MAX_EXIF_PAYLOAD_LENGTH - 1)
    return MMAL_EINVAL;

  exif_param->hdr.id = MMAL_PARAMETER_EXIF;

  strncpy((char *)exif_param->data, exif_tag, MAX_EXIF_PAYLOAD_LENGTH - 1);

  exif_param->hdr.size =
      sizeof(MMAL_PARAMETER_EXIF_T) + strlen((char *)exif_param->data);

  status = mmal_port_parameter_set(state->encoder_component->output[0],
                                   &exif_param->hdr);

  free(exif_param);

  return status;
}

/**
 * Add a basic set of EXIF tags to the capture
 * Make, Time etc
 *
 * @param state Pointer to state control struct
 *
 */
static void add_exif_tags(RASPISTILL_STATE *state) {
  time_t rawtime;
  struct tm *timeinfo;
  char model_buf[32];
  char time_buf[32];
  char exif_buf[128];
  int i;

  snprintf(model_buf, 32, "IFD0.Model=RP_%s", state->camera_name);
  add_exif_tag(state, model_buf);
  add_exif_tag(state, "IFD0.Make=RaspberryPi");

  time(&rawtime);
  timeinfo = localtime(&rawtime);

  snprintf(time_buf, sizeof(time_buf), "%04d:%02d:%02d %02d:%02d:%02d",
           timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
           timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);

  snprintf(exif_buf, sizeof(exif_buf), "EXIF.DateTimeDigitized=%s", time_buf);
  add_exif_tag(state, exif_buf);

  snprintf(exif_buf, sizeof(exif_buf), "EXIF.DateTimeOriginal=%s", time_buf);
  add_exif_tag(state, exif_buf);

  snprintf(exif_buf, sizeof(exif_buf), "IFD0.DateTime=%s", time_buf);
  add_exif_tag(state, exif_buf);

  // Now send any user supplied tags

  for (i = 0; i < state->numExifTags && i < MAX_USER_EXIF_TAGS; i++) {
    if (state->exifTags[i]) {
      add_exif_tag(state, state->exifTags[i]);
    }
  }
}

/**
 * Stores an EXIF tag in the state, incrementing various pointers as necessary.
 * Any tags stored in this way will be added to the image file when
 *add_exif_tags
 * is called
 *
 * Will not store if run out of storage space
 *
 * @param state Pointer to state control struct
 * @param exif_tag EXIF tag string
 *
 */
static void store_exif_tag(RASPISTILL_STATE *state, const char *exif_tag) {
  if (state->numExifTags < MAX_USER_EXIF_TAGS) {
    state->exifTags[state->numExifTags] = exif_tag;
    state->numExifTags++;
  }
}

/**
 * Connect two specific ports together
 *
 * @param output_port Pointer the output port
 * @param input_port Pointer the input port
 * @param Pointer to a mmal connection pointer, reassigned if function
 *successful
 * @return Returns a MMAL_STATUS_T giving result of operation
 *
 */
static MMAL_STATUS_T connect_ports(MMAL_PORT_T *output_port,
                                   MMAL_PORT_T *input_port,
                                   MMAL_CONNECTION_T **connection) {
  MMAL_STATUS_T status;

  status = mmal_connection_create(connection, output_port, input_port,
                                  MMAL_CONNECTION_FLAG_TUNNELLING |
                                      MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT);

  if (status == MMAL_SUCCESS) {
    status = mmal_connection_enable(*connection);
    if (status != MMAL_SUCCESS)
      mmal_connection_destroy(*connection);
  }

  return status;
}

/**
 * Checks if specified port is valid and enabled, then disables it
 *
 * @param port  Pointer the port
 *
 */
static void check_disable_port(MMAL_PORT_T *port) {
  if (port && port->is_enabled)
    mmal_port_disable(port);
}

/**
 * Handler for sigint signals
 *
 * @param signal_number ID of incoming signal.
 *
 */
static void signal_handler(int signal_number) {
  if (signal_number == SIGUSR1) {
    // Handle but ignore - prevents us dropping out if started in none-signal
    // mode
    // and someone sends us the USR1 signal anyway
  } else {
    // Going to abort on all other signals
    vcos_log_error("Aborting program\n");
    exit(130);
  }
}

/**
 * Function to wait in various ways (depending on settings) for the next frame
 *
 * @param state Pointer to the state data
 * @param [in][out] frame The last frame number, adjusted to next frame number
 *on output
 * @return !0 if to continue, 0 if reached end of run
 */
static int wait_for_next_frame(RASPISTILL_STATE *state, int *frame) {
  static int64_t complete_time = -1;
  int keep_running = 1;

  int64_t current_time = vcos_getmicrosecs64() / 1000;

  if (complete_time == -1)
    complete_time = current_time + state->timeout;

  // if we have run out of time, flag we need to exit
  // If timeout = 0 then always continue
  if (current_time >= complete_time && state->timeout != 0)
    keep_running = 0;

  switch (state->frameNextMethod) {
  case FRAME_NEXT_SINGLE:
    // simple timeout for a single capture
    vcos_sleep(5000);
    return 0;

  case FRAME_NEXT_FOREVER: {
    *frame += 1;

    // Have a sleep so we don't hog the CPU.
    vcos_sleep(10000);

    // Run forever so never indicate end of loop
    return 1;
  }

  case FRAME_NEXT_TIMELAPSE: {
    static int64_t next_frame_ms = -1;

    // Always need to increment by at least one, may add a skip later
    *frame += 1;

    if (next_frame_ms == -1) {
      vcos_sleep(state->timelapse);

      // Update our current time after the sleep
      current_time = vcos_getmicrosecs64() / 1000;

      // Set our initial 'next frame time'
      next_frame_ms = current_time + state->timelapse;
    } else {
      int64_t this_delay_ms = next_frame_ms - current_time;

      if (this_delay_ms < 0) {
        // We are already past the next exposure time
        if (-this_delay_ms < state->timelapse / 2) {
          // Less than a half frame late, take a frame and hope to catch up next
          // time
          next_frame_ms += state->timelapse;
          vcos_log_error("Frame %d is %d ms late", *frame,
                         (int)(-this_delay_ms));
        } else {
          int nskip = 1 + (-this_delay_ms) / state->timelapse;
          vcos_log_error("Skipping frame %d to restart at frame %d", *frame,
                         *frame + nskip);
          *frame += nskip;
          this_delay_ms += nskip * state->timelapse;
          vcos_sleep(this_delay_ms);
          next_frame_ms += (nskip + 1) * state->timelapse;
        }
      } else {
        vcos_sleep(this_delay_ms);
        next_frame_ms += state->timelapse;
      }
    }

    return keep_running;
  }

  case FRAME_NEXT_KEYPRESS: {
    int ch;

    if (state->verbose)
      fprintf(stderr, "Press Enter to capture, X then ENTER to exit\n");

    ch = getchar();
    *frame += 1;
    if (ch == 'x' || ch == 'X')
      return 0;
    else {
      return keep_running;
    }
  }

  case FRAME_NEXT_IMMEDIATELY: {
    // Not waiting, just go to next frame.
    // Actually, we do need a slight delay here otherwise exposure goes
    // badly wrong since we never allow it frames to work it out
    // This could probably be tuned down.
    // First frame has a much longer delay to ensure we get exposure to a steady
    // state
    if (*frame == 0)
      vcos_sleep(1000);
    else
      vcos_sleep(30);

    *frame += 1;

    return keep_running;
  }

  case FRAME_NEXT_GPIO: {
    // Intended for GPIO firing of a capture
    return 0;
  }

  case FRAME_NEXT_SIGNAL: {
    // Need to wait for a SIGUSR1 signal
    sigset_t waitset;
    int sig;
    int result = 0;

    sigemptyset(&waitset);
    sigaddset(&waitset, SIGUSR1);

    // We are multi threaded because we use mmal, so need to use the pthread
    // variant of procmask to block SIGUSR1 so we can wait on it.
    pthread_sigmask(SIG_BLOCK, &waitset, NULL);

    if (state->verbose) {
      fprintf(stderr, "Waiting for SIGUSR1 to initiate capture\n");
    }

    result = sigwait(&waitset, &sig);

    if (state->verbose) {
      if (result == 0) {
        fprintf(stderr, "Received SIGUSR1\n");
      } else {
        fprintf(stderr, "Bad signal received - error %d\n", errno);
      }
    }

    *frame += 1;

    return keep_running;
  }
  } // end of switch

  // Should have returned by now, but default to timeout
  return keep_running;
}

static void rename_file(RASPISTILL_STATE *state, FILE *output_file,
                        const char *final_filename, const char *use_filename,
                        int frame) {
  MMAL_STATUS_T status;

  fclose(output_file);
  vcos_assert(use_filename != NULL && final_filename != NULL);
  if (0 != rename(use_filename, final_filename)) {
    vcos_log_error("Could not rename temp file to: %s; %s", final_filename,
                   strerror(errno));
  }
}

/**
 * main
 */
int main(int argc, const char **argv) {
  // Our main data storage vessel..
  RASPISTILL_STATE state;
  int exit_code = EX_OK;
  int num;
  int q;

  MMAL_STATUS_T status = MMAL_SUCCESS;
  MMAL_PORT_T *camera_preview_port = NULL;
  MMAL_PORT_T *camera_video_port = NULL;
  MMAL_PORT_T *camera_still_port = NULL;
  MMAL_PORT_T *preview_input_port = NULL;
  MMAL_PORT_T *encoder_input_port = NULL;
  MMAL_PORT_T *encoder_output_port = NULL;

  bcm_host_init();

  wiringPiSetupGpio();

  // Register our application with the logging system
  vcos_log_register("RaspiStill", VCOS_LOG_CATEGORY);

  signal(SIGINT, signal_handler);

  // Disable USR1 for the moment - may be reenabled if go in to signal capture
  // mode
  signal(SIGUSR1, SIG_IGN);

  default_status(&state);

  // Do we have any parameters

  if (state.verbose) {
    fprintf(stderr, "\n%s Camera App %s\n\n", basename(argv[0]),
            VERSION_STRING);

    dump_status(&state);
  }

  // OK, we have a nice set of parameters. Now set up our components
  // We have three components. Camera, Preview and encoder.
  // Camera and encoder are different in stills/video, but preview
  // is the same so handed off to a separate module

  if ((status = create_camera_component(&state)) != MMAL_SUCCESS) {
    vcos_log_error("%s: Failed to create camera component", __func__);
    exit_code = EX_SOFTWARE;
  } else if ((status = raspipreview_create(&state.preview_parameters)) !=
             MMAL_SUCCESS) {
    vcos_log_error("%s: Failed to create preview component", __func__);
    destroy_camera_component(&state);
    exit_code = EX_SOFTWARE;
  } else if ((status = create_encoder_component(&state)) != MMAL_SUCCESS) {
    vcos_log_error("%s: Failed to create encode component", __func__);
    raspipreview_destroy(&state.preview_parameters);
    destroy_camera_component(&state);
    exit_code = EX_SOFTWARE;
  } else {
    PORT_USERDATA callback_data;

    if (state.verbose)
      fprintf(stderr, "Starting component connection stage\n");

    camera_preview_port =
        state.camera_component->output[MMAL_CAMERA_PREVIEW_PORT];
    camera_video_port = state.camera_component->output[MMAL_CAMERA_VIDEO_PORT];
    camera_still_port =
        state.camera_component->output[MMAL_CAMERA_CAPTURE_PORT];
    encoder_input_port = state.encoder_component->input[0];
    encoder_output_port = state.encoder_component->output[0];

    if (state.verbose)
      fprintf(stderr, "Connecting camera preview port to video render.\n");

    // Note we are lucky that the preview and null sink components use the same
    // input port
    // so we can simple do this without conditionals
    preview_input_port = state.preview_parameters.preview_component->input[0];

    // Connect camera to preview (which might be a null_sink if no preview
    // required)
    status = connect_ports(camera_preview_port, preview_input_port,
                           &state.preview_connection);

    if (status == MMAL_SUCCESS) {
      VCOS_STATUS_T vcos_status;

      if (state.verbose)
        fprintf(stderr,
                "Connecting camera stills port to encoder input port\n");

      // Now connect the camera to the encoder
      status = connect_ports(camera_still_port, encoder_input_port,
                             &state.encoder_connection);

      if (status != MMAL_SUCCESS) {
        vcos_log_error(
            "%s: Failed to connect camera video port to encoder input",
            __func__);
        goto error;
      }

      // Set up our userdata - this is passed though to the callback where we
      // need the information.
      // Null until we open our filename
      callback_data.file_handle = NULL;
      callback_data.pstate = &state;
      vcos_status = vcos_semaphore_create(&callback_data.complete_semaphore,
                                          "RaspiStill-sem", 0);

      vcos_assert(vcos_status == VCOS_SUCCESS);

      if (status != MMAL_SUCCESS) {
        vcos_log_error("Failed to setup encoder output");
        goto error;
      }
      if (1) {
        printf("Start capture of video port...\n");
        if (mmal_port_parameter_set_boolean(
                camera_video_port, MMAL_PARAMETER_CAPTURE, 1) != MMAL_SUCCESS) {
          printf("%s: Failed to start capture\n", __func__);
        }
        printf("Start capture of video port... OK\n");
      }

      pinMode(21, OUTPUT); // on the left side we control this pin, we read the value back in this program to have the same effect as on the right pi
      int input = 0;
      int frame = 0;
      while (1) {
        outputFileFD=-1;
        do {
          input = digitalRead(21);
        } while (input == 0);

        if (mmal_port_parameter_set_uint32(state.camera_component->control,
                                           MMAL_PARAMETER_SHUTTER_SPEED,
                                           0) != MMAL_SUCCESS)
          vcos_log_error("Unable to set shutter speed");

        encoder_output_port->userdata =
            (struct MMAL_PORT_USERDATA_T *)&callback_data;
        status = mmal_port_enable(encoder_output_port, encoder_buffer_callback);

        // Enable the encoder output port and tell it its callback function

        // Send all the buffers to the encoder output port
        num = mmal_queue_length(state.encoder_pool->queue);

        for (q = 0; q < num; q++) {
          MMAL_BUFFER_HEADER_T *buffer =
              mmal_queue_get(state.encoder_pool->queue);

          if (!buffer)
            vcos_log_error("Unable to get a required buffer %d from pool queue",
                           q);

          if (mmal_port_send_buffer(encoder_output_port, buffer) !=
              MMAL_SUCCESS)
            vcos_log_error(
                "Unable to send a buffer to encoder output port (%d)", q);
        }

        if (state.verbose)
          fprintf(stderr, "Starting capture \n");
        if (frame == 0) {
          mmal_port_parameter_set_boolean(state.camera_component->control,
                                          MMAL_PARAMETER_CAMERA_BURST_CAPTURE,
                                          1);
        }
        frame++;

        if (mmal_port_parameter_set_boolean(
                camera_still_port, MMAL_PARAMETER_CAPTURE, 1) != MMAL_SUCCESS) {
          vcos_log_error("%s: Failed to start capture", __func__);
        }

        vcos_semaphore_wait(&callback_data.complete_semaphore);
        status = mmal_port_disable(encoder_output_port);

        do {
          input = digitalRead(21);
        } while (input == 1);
      }

      vcos_semaphore_delete(&callback_data.complete_semaphore);
    }
  }

error:

  // mmal_status_to_int(status);

  if (state.verbose)
    fprintf(stderr, "Closing down\n");

  // Disable all our ports that are not handled by connections
  check_disable_port(camera_video_port);
  check_disable_port(encoder_output_port);

  if (state.preview_connection)
    mmal_connection_destroy(state.preview_connection);

  if (state.encoder_connection)
    mmal_connection_destroy(state.encoder_connection);

  /* Disable components */
  if (state.encoder_component)
    mmal_component_disable(state.encoder_component);

  if (state.preview_parameters.preview_component)
    mmal_component_disable(state.preview_parameters.preview_component);

  if (state.camera_component)
    mmal_component_disable(state.camera_component);

  destroy_encoder_component(&state);
  raspipreview_destroy(&state.preview_parameters);
  destroy_camera_component(&state);

  if (state.verbose)
    fprintf(stderr, "Close down completed, all components disconnected, "
                    "disabled and destroyed\n\n");

  return exit_code;
}
