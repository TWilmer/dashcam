cmake_minimum_required(VERSION 2.8)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Wall ")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -Wall ")
add_compile_options(-std=c++11)

SET(COMPILE_DEFINITIONS -Werror)

include_directories(/opt/vc/include)
include_directories(/opt/vc/include/interface/vcos/pthreads)
include_directories(/opt/vc/include/interface/vmcs_host)
include_directories(/opt/vc/include/interface/vmcs_host/linux)

include_directories(/home/pi/userland)
include_directories(/opt/vc/src/hello_pi/libs/vgfont)

link_directories(/opt/vc/lib)
link_directories(/opt/vc/src/hello_pi/libs/vgfont)

add_executable(dashcam dashcam.cpp RaspiPreview.c RaspiCamControl.c)
add_executable(dashcamR dashcam_right.cpp RaspiPreview.c RaspiCamControl.c)

find_package( OpenCV REQUIRED )

target_link_libraries(dashcam mmal_core mmal_util mmal_vc_client vcos bcm_host  ${OpenCV_LIBS}  openmaxil EGL pthread rt m)
target_link_libraries(dashcamR mmal_core mmal_util mmal_vc_client vcos bcm_host  ${OpenCV_LIBS}  openmaxil EGL pthread rt m wiringPi)

add_executable(dashgrab dashgrab.cpp)
target_link_libraries(dashgrab mmal_core mmal_util mmal_vc_client vcos bcm_host  ${OpenCV_LIBS}  openmaxil EGL pthread rt m)
