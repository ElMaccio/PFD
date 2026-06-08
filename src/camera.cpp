//
// Created by Administrator on 23.02.2026.
//

#include "camera.h"

std::queue<Request*> MCamera::completed_requests;
std::mutex MCamera::request_mutex;
std::condition_variable MCamera::request_cv;

void MCamera::init_camera() {
    cm = std::make_unique<CameraManager>();
    cm->start();

    auto cameras = cm->cameras();
    if (cameras.empty()) {
        std::cerr << "Nie znaleziono kamery." << std::endl;
        exit(1);
    }
    camera = cameras[0];
    camera->acquire();

    std::unique_ptr<CameraConfiguration> config = camera->generateConfiguration({ StreamRole::VideoRecording });
    StreamConfiguration &stream_cfg = config->at(0);
    stream_cfg.pixelFormat = PixelFormat::fromString("XRGB8888");
    stream_cfg.size = Size(1280, 720);   // przykładowy rozmiar, można zmienić
    config->validate();
    camera->configure(config.get());

    video_stream = config->at(0).stream();
    stream_size = config->at(0).size;
    stride_value = config->at(0).stride;   // zapamiętaj stride

    allocator = std::make_unique<FrameBufferAllocator>(camera);
    int ret = allocator->allocate(video_stream);
    if (ret < 0) {
        std::cerr << "Nie udało się zaalokować buforów kamery." << std::endl;
        exit(1);
    }

    const std::vector<std::unique_ptr<FrameBuffer>> &buffers = allocator->buffers(video_stream);
    for (unsigned int i = 0; i < buffers.size(); ++i) {
        std::unique_ptr<Request> req = camera->createRequest();
        req->addBuffer(video_stream, buffers[i].get());
        requests.push_back(std::move(req));

        // Wypełnij mapę fd -> FrameBuffer*
        int fd = buffers[i]->planes()[0].fd.get();
        fd_to_buffer[fd] = buffers[i].get();
    }

    // Podłącz callback zakończenia żądania
    camera->requestCompleted.connect(MCamera::requestComplete);

    camera->start();
    std::cout << "Kamera uruchomiona, użyto " << buffers.size() << " buforów." << std::endl;
}

// ----------------------------------------------------------------------
// Funkcja wywoływana po zakończeniu żądania (callback)
void MCamera::requestComplete(Request *request) {
    // Debug: licznik klatek – odkomentuj, jeśli chcesz zobaczyć, czy kamera działa
    // static int frame_count = 0;
    // std::cout << "Klatka " << ++frame_count << " zakończona" << std::endl;

    std::lock_guard<std::mutex> lock(request_mutex);
    completed_requests.push(request);
    request_cv.notify_one();
}

MCamera::MCamera() {
    this->init_camera();
}
