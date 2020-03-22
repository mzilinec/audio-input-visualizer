#include <cstdio>
#include <iostream>
#include <cstdlib>
#include <thread>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <signal.h>
#include <unistd.h>
#include "portaudio.h"


#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/buffers_iterator.hpp>

#include <functional>


#define SAMPLE_RATE 44100
#define FRAMES_PER_BUFFER 256
#define N_CHANNELS 1
#define SEND_FREQUENCY 10



namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>



std::mutex *mutex = new std::mutex();
bool *is_done = new bool();
std::condition_variable *has_changed = new std::condition_variable();
std::queue<std::pair<float*, size_t>> *samples_to_process = new std::queue<std::pair<float*, size_t>>();



class Data {
  public:
    Data() {
        samples = init_buffer();
    }
    ~Data() {}
    float * init_buffer() {
        size_t max_frame = (size_t) SAMPLE_RATE / SEND_FREQUENCY;
        length = max_frame * N_CHANNELS;
        // length += (FRAMES_PER_BUFFER - (length % FRAMES_PER_BUFFER));
        length *= 2; // timing is not precise, use 2x larger buffer to be sure
        offset = 0;
        return new float [length * sizeof(float)]();
    }
    std::pair<float*, size_t> swap_buffer() {
        if (samples != NULL) {
            float *old_buffer = samples;
            size_t len = offset;
            samples = init_buffer();
            return std::make_pair(old_buffer, len);
        } else {
            throw std::exception();
        }
    }
    size_t length;
    size_t offset;
    float *samples;
};

/* This routine will be called by the PortAudio engine when audio is needed.
   It may called at interrupt level on some machines so don't do anything
   that could mess up the system like calling malloc() or free().
*/ 
static int audioCallback( const void *inputBuffer, void *outputBuffer,
                           unsigned long framesPerBuffer,
                           const PaStreamCallbackTimeInfo* timeInfo,
                           PaStreamCallbackFlags statusFlags,
                           void *userData )
{
    Data *data = (Data*) userData;
    float *input = (float*) inputBuffer;
    for (size_t i = 0; i < framesPerBuffer; i++) {
        for (size_t j = 0; j < N_CHANNELS; j++) {
            data->samples[data->offset++] = input[N_CHANNELS * i + j];
        }
    }
    return 0;
}


void checkError(int err) {
    if (err != paNoError) {
        printf("[ERROR] PortAudio error: %s\n", Pa_GetErrorText( err ));
        exit(1);
    }
}

class Sender {
public:
    std::mutex *mMutex;
    std::condition_variable *mCondVar;
    std::queue<std::pair<float*, size_t>> *mSamples;
    tcp::socket *mSocket;
    bool *mFinished;
    websocket::stream<tcp::socket> ws;

    Sender(std::mutex *mutex, std::condition_variable *cond_var, std::queue<std::pair<float*, size_t>> *samples, bool *finished, tcp::socket *socket) 
    : mMutex(mutex), mCondVar(cond_var), mSamples(samples), mFinished(finished), mSocket(socket), ws{std::move(*socket)}
    {
        // Accept the websocket handshake
        ws.accept();
    }

    ~Sender() {
        delete mSocket;
    }

    void sendData(const float *arr, size_t len) {
        // TODO: time this, must not take >1s
        // std::cout << len << " " << sizeof(arr), std::endl;
        auto buffer = boost::asio::buffer(arr, len * sizeof(float));
        // ws.read(buffer);
        ws.text(false);//ws.got_text());
        ws.write(buffer);
    }

    void mainLoop() {
        while (true) {
            std::unique_lock<std::mutex> lock(*mMutex);
            mCondVar->wait(lock, [this] {return !mSamples->empty() || *mFinished;});
            while (!mSamples->empty()) {
                // std::cout << "Sending data" << std::endl;
                std::pair<float*, size_t> tuple = mSamples->front();
                mSamples->pop();
                try {
                    sendData(tuple.first, tuple.second);
                } catch (boost::wrapexcept<boost::system::system_error> ex) {
                    std::cout << ex.what() << std::endl;
                    return;
                }
                delete [] tuple.first;
            }
            if (*mFinished) {
                std::cout << "All data sent" << std::endl;
                break;
            }
        }
    }
};


class ConnectionListener {
    // Listens for new clients and establishes connections.

    std::vector<std::thread*> mSenderThreads;
    const std::string &mAddr;
    const unsigned short mPort;

  public:
    ConnectionListener(const std::string &addr, const unsigned short port)
    : mAddr(addr), mPort(port) {}

    ~ConnectionListener() {
        for (std::thread *thread : mSenderThreads) {
            thread->join();
            delete thread;
        }
    }

    void mainLoop() {
        waitForConnections();
    }

    /*tcp::socket*/ void waitForConnections() { //init_websocket() {
        auto const address = net::ip::make_address(mAddr);
        auto const port = static_cast<unsigned short>(mPort);
        // The io_context is required for all I/O
        net::io_context ioc{1};
        // The acceptor receives incoming connections
        tcp::acceptor acceptor{ioc, {address, port}};
        // This will receive the new connection
        while (true) {
            tcp::socket *socket = new tcp::socket{ioc};
            std::cout << "Waiting for the client" << std::endl;
            // Block until we get a connection
            acceptor.accept(*socket);
            std::cout << "Connected to client" << std::endl;
            initClient(socket);
            if (*is_done) {
                std::cout << "Stopping listener" << std::endl;
                return;
            }
        }
        // return socket;
    }

    void initClient(tcp::socket *socket) {
        Sender *sender = new Sender(mutex, has_changed, samples_to_process, is_done, socket);
        std::thread *senderThread = new std::thread(&Sender::mainLoop, sender);
        mSenderThreads.push_back(senderThread);
    }
};


void signal_handler(int signal) {
    std::cout << "Received signal " << signal << ", exiting" << std::endl;
    std::unique_lock<std::mutex> lock = std::unique_lock<std::mutex>(*mutex);
    *is_done = true;
}


void read_raw_audio() {
    int err = Pa_Initialize();
    checkError(err);

    PaStream *stream;
    Data *data = new Data();

    err = Pa_OpenDefaultStream(&stream, N_CHANNELS, 0, paFloat32, SAMPLE_RATE, FRAMES_PER_BUFFER, audioCallback, data);
    checkError(err);
    std::cout << "Audio init OK" << std::endl;

    while (true) {
        err = Pa_StartStream(stream);
        checkError(err);
        Pa_Sleep(1000.0 / SEND_FREQUENCY);
        err = Pa_StopStream(stream);
        checkError(err);
        // std::cout << data->offset << " " << data->length << std::endl;
        {
            std::lock_guard<std::mutex> guard(*mutex);
            samples_to_process->push(data->swap_buffer());
            has_changed->notify_one();
            if (*is_done) {
                break;                
            }
        }
    }

    err = Pa_CloseStream(stream);
    checkError(err);

    err = Pa_Terminate();
    checkError(err);
}


int main(int argc, char** argv) {

    int c;
    uint port = 2020;
    opterr = 0;

    while ((c = getopt(argc, argv, ":p:")) != -1)
        switch (c) {
            case 'p': port = atoi(optarg); break;
            case ':': std::cout << "Option " << char(optopt) << " needs a value" << std::endl; exit(1);
            case '?': std::cout << "Unknown option " << char(optopt) << std::endl; exit(1);
            default:
                abort();
        }
    
    if (port < 1 || port > 65535) {
        std::cout << "Invalid port" << std::endl;
        exit(1);
    }

    // signal(SIGINT, signal_handler); // TODO: let's use ctrl+c for now, see below
    *is_done = false;

    ConnectionListener *listener = new ConnectionListener("127.0.0.1", port);
    std::thread listenerThread(&ConnectionListener::mainLoop, listener);

    read_raw_audio();

    // TODO: how to stop blocking acceptor?
    listenerThread.join();
    delete listener;

    return 0;
}
