//
// Created by Administrator on 23.02.2026.
//

#ifndef ISRAEL_CAMERA_H
#define ISRAEL_CAMERA_H

#include <libcamera/libcamera.h>
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>

using namespace libcamera;

class MCamera {
public:
    std::unique_ptr<CameraManager> cm;
    std::shared_ptr<Camera> camera;
    std::unique_ptr<FrameBufferAllocator> allocator;
    Stream *video_stream = nullptr;
    std::vector<std::unique_ptr<Request>> requests;
    Size stream_size;
    unsigned int stride_value = 0;

    // Mapa FD -> nie-const FrameBuffer* do ponownego dodania bufora po reuse()
    std::map<int, FrameBuffer*> fd_to_buffer;

    // Kolejka gotowych żądań (do asynchronicznej obsługi)
    static std::queue<Request*> completed_requests;
    static std::mutex request_mutex;
    static std::condition_variable request_cv;

    static void requestComplete(Request *request);
    MCamera();
private:
    void init_camera();
};


#endif //ISRAEL_CAMERA_H