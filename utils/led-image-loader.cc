// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
// Copyright (C) 2015 Henner Zeller <h.zeller@acm.org>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation version 2.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://gnu.org/licenses/gpl-2.0.txt>

// To use this image viewer, first get image-magick development files
// $ sudo apt-get install libgraphicsmagick++-dev libwebp-dev
//
// Then compile with
// $ make led-image-viewer

#include "led-matrix.h"
#include "transformer.h"
#include "content-streamer.h"

#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

const int vsync_multiple = 1;

#include <Magick++.h>
#include <magick/image.h>

#include <iostream>

#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>

#include <sstream>

#include <math.h>

#define DURATION_IMG 1500 // ms needed to show an image

using namespace std;

using rgb_matrix::GPIO;
using rgb_matrix::Canvas;
using rgb_matrix::FrameCanvas;
using rgb_matrix::RGBMatrix;
using rgb_matrix::StreamReader;

typedef int64_t tmillis_t;
static const tmillis_t distant_future = (1LL<<40); // that is a while.

struct ImageParams {
  ImageParams() : anim_duration_ms(distant_future), wait_ms(1500),
                  anim_delay_ms(-1), loops(-1) {}
  tmillis_t anim_duration_ms;  // If this is an animation, duration to show.
  tmillis_t wait_ms;           // Regular image: duration to show.
  tmillis_t anim_delay_ms;     // Animation delay override.
  int loops;
};

struct FileInfo {
  ImageParams params;      // Each file might have specific timing settings
  bool is_multi_frame;
  rgb_matrix::StreamIO *content_stream;
};

volatile bool interrupt_received = false;
static void InterruptHandler(int signo) {
  interrupt_received = true;
}

static tmillis_t GetTimeInMillis() {
  struct timeval tp;
  gettimeofday(&tp, NULL);
  return tp.tv_sec * 1000 + tp.tv_usec / 1000;
}

static void SleepMillis(tmillis_t milli_seconds) {
  if (milli_seconds <= 0) return;
  struct timespec ts;
  ts.tv_sec = milli_seconds / 1000;
  ts.tv_nsec = (milli_seconds % 1000) * 1000000;
  nanosleep(&ts, NULL);
}

static void StoreInStream(const Magick::Image &img, int delay_time_us,
                          bool do_center,
                          rgb_matrix::FrameCanvas *scratch,
                          rgb_matrix::StreamWriter *output) {
  scratch->Clear();
  const int x_offset = do_center ? (scratch->width() - img.columns()) / 2 : 0;
  const int y_offset = do_center ? (scratch->height() - img.rows()) / 2 : 0;
  for (size_t y = 0; y < img.rows(); ++y) {
    for (size_t x = 0; x < img.columns(); ++x) {
      const Magick::Color &c = img.pixelColor(x, y);
      if (c.alphaQuantum() < 256) {
        scratch->SetPixel(x + x_offset, y + y_offset,
                          ScaleQuantumToChar(c.redQuantum()),
                          ScaleQuantumToChar(c.greenQuantum()),
                          ScaleQuantumToChar(c.blueQuantum()));
      }
    }
  }
  output->Stream(*scratch, delay_time_us);
}

static void CopyStream(rgb_matrix::StreamReader *r,
                       rgb_matrix::StreamWriter *w,
                       rgb_matrix::FrameCanvas *scratch) {
  uint32_t delay_us;
  while (r->GetNext(scratch, &delay_us)) {
    w->Stream(*scratch, delay_us);
  }
}

// Load still image or animation.
// Scale, so that it fits in "width" and "height" and store in "result".
static bool LoadImageAndScale(const char *filename,
                              int target_width, int target_height,
                              bool fill_width, bool fill_height,
                              std::vector<Magick::Image> *result,
                              std::string *err_msg) {
  std::vector<Magick::Image> frames;
  try {
    readImages(&frames, filename);
  } catch (std::exception& e) {
    if (e.what()) *err_msg = e.what();
    return false;
  }
  if (frames.size() == 0) {
    fprintf(stderr, "No image found.");
    return false;
  }

  // Put together the animation from single frames. GIFs can have nasty
  // disposal modes, but they are handled nicely by coalesceImages()
  if (frames.size() > 1) {
    Magick::coalesceImages(result, frames.begin(), frames.end());
  } else {
    result->push_back(frames[0]);   // just a single still image.
  }

  const int img_width = (*result)[0].columns();
  const int img_height = (*result)[0].rows();
  const float width_fraction = (float)target_width / img_width;
  const float height_fraction = (float)target_height / img_height;
  if (fill_width && fill_height) {
    // Scrolling diagonally. Fill as much as we can get in available space.
    // Largest scale fraction determines that.
    const float larger_fraction = (width_fraction > height_fraction)
      ? width_fraction
      : height_fraction;
    target_width = (int) roundf(larger_fraction * img_width);
    target_height = (int) roundf(larger_fraction * img_height);
  }
  else if (fill_height) {
    // Horizontal scrolling: Make things fit in vertical space.
    // While the height constraint stays the same, we can expand to full
    // width as we scroll along that axis.
    target_width = (int) roundf(height_fraction * img_width);
  }
  else if (fill_width) {
    // dito, vertical. Make things fit in horizontal space.
    target_height = (int) roundf(width_fraction * img_height);
  }

  for (size_t i = 0; i < result->size(); ++i) {
    (*result)[i].scale(Magick::Geometry(target_width, target_height));
  }

  return true;
}

void DisplayAnimation(const FileInfo *file,
                      RGBMatrix *matrix, FrameCanvas *offscreen_canvas,
                      int vsync_multiple) {
  const tmillis_t duration_ms = (file->is_multi_frame
                                 ? file->params.anim_duration_ms
                                 : file->params.wait_ms);
  rgb_matrix::StreamReader reader(file->content_stream);
  int loops = file->params.loops;
  const tmillis_t end_time_ms = GetTimeInMillis() + duration_ms;
  const tmillis_t override_anim_delay = file->params.anim_delay_ms;
  for (int k = 0;
       (loops < 0 || k < loops)
         && !interrupt_received
         && GetTimeInMillis() < end_time_ms;
       ++k) {
    uint32_t delay_us = 0;
    while (!interrupt_received && GetTimeInMillis() <= end_time_ms
           && reader.GetNext(offscreen_canvas, &delay_us)) {
      const tmillis_t anim_delay_ms =
        override_anim_delay >= 0 ? override_anim_delay : delay_us / 1000;
      const tmillis_t start_wait_ms = GetTimeInMillis();
      offscreen_canvas = matrix->SwapOnVSync(offscreen_canvas, vsync_multiple);
      const tmillis_t time_already_spent = GetTimeInMillis() - start_wait_ms;
      SleepMillis(anim_delay_ms - time_already_spent);
    }
    reader.Rewind();
  }
}

static int usage(const char *progname) {
  fprintf(stderr, "usage: %s [options] <image> [option] [<image> ...]\n",
          progname);

  fprintf(stderr, "Options:\n"
          "\t-O<streamfile>            : Output to stream-file instead of matrix (Don't need to be root).\n"
          "\t-C                        : Center images.\n"

          "\nThese options affect images following them on the command line:\n"
          "\t-w<seconds>               : Regular image: "
          "Wait time in seconds before next image is shown (default: 1.5).\n"
          "\t-t<seconds>               : "
          "For animations: stop after this time.\n"
          "\t-l<loop-count>            : "
          "For animations: number of loops through a full cycle.\n"
          "\t-D<animation-delay-ms>    : "
          "For animations: override the delay between frames given in the\n"
          "\t                            gif/stream animation with this value. Use -1 to use default value.\n"

          "\nOptions affecting display of multiple images:\n"
          "\t-f                        : "
          "Forever cycle through the list of files on the command line.\n"
          "\t-s                        : If multiple images are given: shuffle.\n"
          "\nDisplay Options:\n"
          "\t-V<vsync-multiple>        : Expert: Only do frame vsync-swaps on multiples of refresh (default: 1)\n"
          "\t-L                        : Large display, in which each chain is 'folded down'\n"
          "\t                            in the middle in an U-arrangement to get more vertical space.\n"
          "\t-R<angle>                 : Rotate output; steps of 90 degrees\n"
          );

  fprintf(stderr, "\nGeneral LED matrix options:\n");
  rgb_matrix::PrintMatrixFlags(stderr);

  fprintf(stderr,
          "\nSwitch time between files: "
          "-w for static images; -t/-l for animations\n"
          "Animated gifs: If both -l and -t are given, "
          "whatever finishes first determines duration.\n");

  fprintf(stderr, "\nThe -w, -t and -l options apply to the following images "
          "until a new instance of one of these options is seen.\n"
          "So you can choose different durations for different images.\n");

  return 1;
}

void showImage(FileInfo *file_img, RGBMatrix *matrix, FrameCanvas *offscreen_canvas, tmillis_t executionTime) {
  file_img->params.wait_ms = executionTime;
  file_img->params.anim_duration_ms = executionTime;
  file_img->params.loops = 1;//ceil(executionTime/DURATION_IMG);
  DisplayAnimation(file_img, matrix, offscreen_canvas, vsync_multiple);
  matrix->Clear();
}

bool check_exit(const char *msg) {
	for (int i = 0; i < strlen(msg); ++i) {
		if (msg[i] == '#') 
			return true;
	}
	return false;
}

std::vector<std::string> split(const std::string& s, char seperator)
{
   std::vector<std::string> output;

    std::string::size_type prev_pos = 0, pos = 0;

    while((pos = s.find(seperator, pos)) != std::string::npos)
    {
        std::string substring( s.substr(prev_pos, pos-prev_pos) );

        output.push_back(substring);

        prev_pos = ++pos;
    }

    output.push_back(s.substr(prev_pos, pos-prev_pos)); // Last word

    return output;
}

int main(int argc, char *argv[]) {
  argv[0] = "./led-image-loader";
  argv[1] = "--led-gpio-mapping=adafruit-hat";
  argc = 2;
  std::string prefix("/home/pi/led-images/");
  std::string prefix_file = prefix + std::string("arrow-");
  Magick::InitializeMagick(*argv);

  RGBMatrix::Options matrix_options;
  rgb_matrix::RuntimeOptions runtime_opt;
  if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv,
                                         &matrix_options, &runtime_opt)) {
    return usage(argv[0]);
  }

  bool do_forever = false;
  bool do_center = false;
  bool do_shuffle = false;
  bool large_display = false;  // 64x64 made out of 4 in sequence.
  int angle = -361;

  // We remember ImageParams for each image, which will change whenever
  // there is a flag modifying them. This map keeps track of filenames
  // and their image params (also for unrelated elements of argv[], but doesn't
  // matter).
  // We map the pointer instad of the string of the argv parameter so that
  // we can have two times the same image on the commandline list with different
  // parameters.
  //std::map<const void *, struct ImageParams> filename_params;

  // Set defaults.
  ImageParams img_param;
    
  std::map<int, std::string> mapColors;
  mapColors[0]="red";
  mapColors[1]="green";
  //mapColors[2]="blue";
  //mapColors[3]="orange";

  std::map<int, std::string> mapDirections;
  mapDirections[0] = "top";
  mapDirections[1] = "right";
  mapDirections[2] = "bottom";
  mapDirections[3] = "left";
  mapDirections[4] = "destination";
  mapDirections[5] = "start";

  int num_colors=mapColors.size();
  int num_directions=mapDirections.size();
  std::string color = ""; 
  std::string direction = "";
  std::string file_name_string = "";
  std::map<std::string, FileInfo*> dictionaryFileImages;
  
  // Prepare matrix
  const char *stream_output = NULL;
  runtime_opt.do_gpio_init = (stream_output == NULL);
  RGBMatrix *matrix = CreateMatrixFromOptions(matrix_options, runtime_opt);
  if(matrix == NULL) {
    return 1;
  }

  FrameCanvas *offscreen_canvas = matrix->CreateFrameCanvas();

  printf("Size: %dx%d. Hardware gpio mapping: %s\n",
         matrix->width(), matrix->height(), matrix_options.hardware_mapping);
  
  for(int c = 0; c<num_colors; c++) {
    
    for(int d = 0; d<num_directions; d++) {
      FileInfo* file_img;
      color = mapColors.at(c);
      direction = mapDirections.at(d);
      file_name_string = prefix_file + color + std::string("-") + direction + std::string(".gif");
    
      // These parameters are needed once we do scrolling.
      const bool fill_width = false;
      const bool fill_height = false;

      // In case the output to stream is requested, set up the stream object.
      rgb_matrix::StreamIO *stream_io = NULL;
      rgb_matrix::StreamWriter *global_stream_writer = NULL;
      if (stream_output) {
        int fd = open(stream_output, O_CREAT|O_WRONLY, 0644);
        if (fd < 0) {
          perror("Couldn't open output stream");
          return 1;
        }
        stream_io = new rgb_matrix::FileStreamIO(fd);
        global_stream_writer = new rgb_matrix::StreamWriter(stream_io);
      }
    
      const tmillis_t start_load = GetTimeInMillis();
      fprintf(stderr, "Loading file...\n");
        
      const char *filename = &file_name_string[0];      
      FileInfo *file_info = NULL;
    
      std::string err_msg;
      std::vector<Magick::Image> image_sequence;
      
      if (LoadImageAndScale(filename, matrix->width(), matrix->height(), fill_width, fill_height, &image_sequence, &err_msg)) {
          
        file_info = new FileInfo();
        file_info->params = img_param;//filename_params[filename];
        file_info->content_stream = new rgb_matrix::MemStreamIO();
        file_info->is_multi_frame = image_sequence.size() > 1;
        rgb_matrix::StreamWriter out(file_info->content_stream);
          
        for (size_t i = 0; i < image_sequence.size(); ++i) {
          const Magick::Image &img = image_sequence[i];
          int64_t delay_time_us;
          if (file_info->is_multi_frame) {
            delay_time_us = img.animationDelay() * 10000; // unit in 1/100s
          } else {
            delay_time_us = file_info->params.wait_ms * 1000;  // single image.
          }
            
          if (delay_time_us <= 0){
            delay_time_us = 100 * 1000;
          }  // 1/10sec
            
          StoreInStream(img, delay_time_us, do_center, offscreen_canvas,
          global_stream_writer ? global_stream_writer : &out);
        }
    
        file_img = file_info;

        fprintf(stderr, "Loading image took %.3fs.\n",
        (GetTimeInMillis() - start_load) / 1000.0);
      
        signal(SIGTERM, InterruptHandler);
        signal(SIGINT, InterruptHandler);
          
        std::vector<int> v = { c, d };
            std::string delim = "$";
        
            std::stringstream keyStream;
            std::copy(v.begin(), v.end(),
                    std::ostream_iterator<int>(keyStream, delim.c_str()));
        
        std::string key = keyStream.str().substr(0, 2*v.size() - 1);
        
        dictionaryFileImages[key] = file_img;
      }
    }
  }

  std::string suffix_file(".gif");

	int client, server;
	int portnum = 6666;
	int bufsize = 1024;
	bool isExit = false;
	char buffer[bufsize];
	socklen_t size;

	struct sockaddr_in serv_addr;

	client = socket(AF_INET, SOCK_STREAM, 0);
	if (client < 0) {
		cout << "\nERROR establishing socket...";
		exit(1);
	}

	cout << "\n--> Socket server created..\n";

	serv_addr.sin_port = htons(portnum);
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htons(INADDR_ANY);


	if (bind(client, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
		cout << "--> ERROR binding connection, the socket has already been established...\n";
		return -1;
	}

	size = sizeof(serv_addr);
	cout << "--> Looking for clients..." << endl;
  while(!isExit) {
    listen(client, 1);

    server = accept(client, (struct sockaddr*)&serv_addr, &size);
    if (server < 0)
          cout << "--> Error on accepting..." << endl;
    else {

      while (server > 0) 
      {
          cout << "--> Connected to the client" << endl;
          cout << "\nEnter # to end the connection\n\n" << endl;

          while (!isExit) {
              cout << "Client: ";
              for(int i = 0;i<bufsize;i++) {
                buffer[i] = 0;
              }
              recv(server, buffer, bufsize, 0);
              cout << buffer << endl;
              if (check_exit(buffer)) 
                break;
              std::string fromPython(buffer);
              std::vector<std::string> msgs = split(fromPython, '$');
              
              std::string key(msgs[0]+std::string("$")+msgs[1]);
              cout << "KEY: " << key << endl;
              auto iter = dictionaryFileImages.find(key);
              if(iter != dictionaryFileImages.end()) {
		cout << "Showing image" << key << endl;
                FileInfo* file_img = iter->second;
                showImage(file_img, matrix, offscreen_canvas, stoi(msgs[2]));
                delete matrix;
                matrix = CreateMatrixFromOptions(matrix_options, runtime_opt);
                
                if(matrix == NULL) {
                  cout << key << "ERROR: matrix is null" << endl;
                  return 1;
                }

                offscreen_canvas = matrix->CreateFrameCanvas();
              } else {
                cout << key << " Not present" << endl;
              }
              //SleepMillis(5000);
          }
          cout << "\nDisconnected..." << endl;
          isExit = false;
          exit(1);
      }
      SleepMillis(5000);
  
    }
  }

  // Here is where it finishes to create the file, hold on and wait
  //FileInfo* file_img = dictionaryFileImages.find(std::string("/home/pi/smart-directions-slave/assets/arrow-green-top.gif"))->second;
  //showImage(file_img, matrix, offscreen_canvas);
  return 0;
}
