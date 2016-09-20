
#include "nodePortAudio.h"
#include <portaudio.h>

#define FRAMES_PER_BUFFER  (256)

int g_initialized = false;
int g_portAudioStreamInitialized = false;
static Nan::Persistent<v8::Function> streamConstructor;

struct PortAudioData {
  unsigned char* buffer;
  unsigned char* nextBuffer;
  int bufferLen;
  int nextLen;
  int bufferIdx;
  int nextIdx;
  int writeIdx;
  int sampleFormat;
  int channelCount;
  PaStream* stream;
  Nan::Persistent<v8::Object> v8Stream;
  Nan::Persistent<v8::Object> protectBuffer;
  Nan::Persistent<v8::Object> protectNext;
  Nan::Callback *writeCallback;
};

void CleanupStreamData(const Nan::WeakCallbackInfo<PortAudioData> &data) {
  printf("Cleaning up stream data.\n");
  PortAudioData *pad = data.GetParameter();
  Nan::SetInternalFieldPointer(Nan::New(pad->v8Stream), 0, NULL);
  // free(pad->hangover);
  delete pad;
}

static int nodePortAudioCallback(
  const void *inputBuffer,
  void *outputBuffer,
  unsigned long framesPerBuffer,
  const PaStreamCallbackTimeInfo* timeInfo,
  PaStreamCallbackFlags statusFlags,
  void *userData);

NAN_METHOD(StreamWriteByte);
NAN_METHOD(StreamWrite);
NAN_METHOD(StreamStart);
NAN_METHOD(StreamStop);
NAN_METHOD(WritableWrite);
/*v8::Handle<v8::Value> stream_writeByte(const v8::Arguments& args);
v8::Handle<v8::Value> stream_write(const v8::Arguments& args);
v8::Handle<v8::Value> stream_start(const v8::Arguments& args);
v8::Handle<v8::Value> stream_stop(const v8::Arguments& args);
void CleanupStreamData(v8::Persistent<v8::Value> obj, void *parameter);
*/
PaError EnsureInitialized() {
  PaError err;

  if(!g_initialized) {
    err = Pa_Initialize();
    if(err != paNoError) {
      return err;
    };
    g_initialized = true;
  }
  return paNoError;
}

NAN_METHOD(Open) {
  PaError err;
  PaStreamParameters outputParameters;
  Nan::MaybeLocal<v8::Object> v8Buffer;
  Nan::MaybeLocal<v8::Object> v8Stream;
  PortAudioData* data;
  int sampleRate;
  char str[1000];
  v8::Local<v8::Value> initArgs[1];
  v8::Local<v8::Value> toEventEmitterArgs[1];
  Nan::MaybeLocal<v8::Value> v8Val;

  printf("Got here!\n");

  v8::Local<v8::Value> argv[] = { Nan::Null(), Nan::Undefined() };

  Nan::MaybeLocal<v8::Object> options = Nan::To<v8::Object>(info[0].As<v8::Object>());
  Nan::Callback *cb = new Nan::Callback(info[1].As<v8::Function>());

  err = EnsureInitialized();
  if(err != paNoError) {
    sprintf(str, "Could not initialize PortAudio: %s", Pa_GetErrorText(err));
    argv[0] = Nan::Error(str);
    goto openDone;
  }

  if(!g_portAudioStreamInitialized) {
    v8::Local<v8::FunctionTemplate> t = Nan::New<v8::FunctionTemplate>();
    t->InstanceTemplate()->SetInternalFieldCount(1);
    t->SetClassName(Nan::New("PortAudioStream").ToLocalChecked());
  //  Nan::SetPrototypeMethod(t, "_write", WriteableWrite);
    streamConstructor.Reset(Nan::GetFunction(t).ToLocalChecked());

    // toEventEmitterArgs[0] = Nan::New(streamConstructor);
    // v8Val = Nan::Get(options.ToLocalChecked(), Nan::New("toEventEmitter").ToLocalChecked());
    // printf("Should be 1: %i\n", Nan::NewInstance(Nan::New(streamConstructor)).ToLocalChecked()->InternalFieldCount());
    // Nan::Call(v8Val.ToLocalChecked().As<v8::Function>(),
    //   options.ToLocalChecked(), 1, toEventEmitterArgs);
    // printf("Should be 1: %i\n", Nan::NewInstance(Nan::New(streamConstructor)).ToLocalChecked()->InternalFieldCount());

    g_portAudioStreamInitialized = true;
  }

  memset(&outputParameters, 0, sizeof(PaStreamParameters));

  outputParameters.device = Pa_GetDefaultOutputDevice();
  if (outputParameters.device == paNoDevice) {
    sprintf(str, "No default output device");
    argv[0] = Nan::Error(str);
    goto openDone;
  }

  v8Val = Nan::Get(options.ToLocalChecked(), Nan::New("channelCount").ToLocalChecked());
  outputParameters.channelCount = Nan::To<int32_t>(v8Val.ToLocalChecked()).FromMaybe(2);
  printf("Channel count %i\n", outputParameters.channelCount);

  v8Val = Nan::Get(options.ToLocalChecked(), Nan::New("sampleFormat").ToLocalChecked());
  switch(Nan::To<int32_t>(v8Val.ToLocalChecked()).FromMaybe(0)) {
  case 8:
    outputParameters.sampleFormat = paInt8;
    break;
  case 16:
    outputParameters.sampleFormat = paInt16;
    break;
  case 24:
    outputParameters.sampleFormat = paInt24;
    break;
  case 32:
    outputParameters.sampleFormat = paInt32;
    break;
  default:
    argv[0] = Nan::Error("Invalid sampleFormat");
    goto openDone;
  }
  outputParameters.suggestedLatency = Pa_GetDeviceInfo( outputParameters.device )->defaultLowOutputLatency;
  outputParameters.hostApiSpecificStreamInfo = NULL;

  v8Val = Nan::Get(options.ToLocalChecked(), Nan::New("sampleRate").ToLocalChecked());
  sampleRate = Nan::To<int32_t>(v8Val.ToLocalChecked()).FromMaybe(44100);

  printf("Sample rate %i\n", sampleRate);

  data = new PortAudioData();
  data->bufferIdx = 0;
  data->nextIdx = 0;
  data->writeIdx = 1;
  data->channelCount = outputParameters.channelCount;
  data->sampleFormat = outputParameters.sampleFormat;

  v8Stream = Nan::New(streamConstructor)->NewInstance();
  printf("Internal field count is %i\n", argv[1].As<v8::Object>()->InternalFieldCount());
  Nan::SetInternalFieldPointer(v8Stream.ToLocalChecked(), 0, data);
  // v8Val = Nan::Get(options.ToLocalChecked(), Nan::New("streamInit").ToLocalChecked());
  // initArgs[0] = v8Stream.ToLocalChecked();
  // Nan::CallAsFunction(Nan::To<v8::Object>(v8Val.ToLocalChecked()).ToLocalChecked(),
  //    v8Stream.ToLocalChecked(), 1, initArgs);
  printf("About to set GC callback.\n");
  data->v8Stream.Reset(v8Stream.ToLocalChecked());
  data->v8Stream.SetWeak(data, CleanupStreamData, Nan::WeakCallbackType::kParameter);
  data->v8Stream.MarkIndependent();

  // v8Buffer = Nan::To<v8::Object>(Nan::Get(v8Stream.ToLocalChecked(),
  //   Nan::New("buffer").ToLocalChecked()).ToLocalChecked());
  // data->buffer = (unsigned char*) node::Buffer::Data(v8Buffer.ToLocalChecked());
  // data->bufferLen = node::Buffer::Length(v8Buffer.ToLocalChecked());

  printf("About to open stream.\n");
  err = Pa_OpenStream(
    &data->stream,
    NULL, // no input
    &outputParameters,
    sampleRate,
    FRAMES_PER_BUFFER,
    paClipOff, // we won't output out of range samples so don't bother clipping them
    nodePortAudioCallback,
    data);
  if(err != paNoError) {
    sprintf(str, "Could not open stream %d", err);
    argv[0] = Nan::Error(str);
    goto openDone;
  }
  data->bufferLen = 6;
  // Nan::Set(v8Stream.ToLocalChecked(), Nan::New("write").ToLocalChecked(),
  //   Nan::GetFunction(Nan::New<v8::FunctionTemplate>(StreamWrite)).ToLocalChecked());
  // Nan::Set(v8Stream.ToLocalChecked(), Nan::New("writeByte").ToLocalChecked(),
  //   Nan::GetFunction(Nan::New<v8::FunctionTemplate>(StreamWriteByte)).ToLocalChecked());
  Nan::Set(v8Stream.ToLocalChecked(), Nan::New("start").ToLocalChecked(),
    Nan::GetFunction(Nan::New<v8::FunctionTemplate>(StreamStart)).ToLocalChecked());
  Nan::Set(v8Stream.ToLocalChecked(), Nan::New("stop").ToLocalChecked(),
    Nan::GetFunction(Nan::New<v8::FunctionTemplate>(StreamStop)).ToLocalChecked());
  Nan::Set(v8Stream.ToLocalChecked(), Nan::New("_write").ToLocalChecked(),
    Nan::GetFunction(Nan::New<v8::FunctionTemplate>(WritableWrite)).ToLocalChecked());

  argv[1] = v8Stream.ToLocalChecked();

openDone:
  cb->Call(2, argv);
  info.GetReturnValue().SetUndefined();
  printf("Stream opened OK.\n");
}
/*
v8::Handle<v8::Value> Open(const v8::Arguments& args) {
  v8::HandleScope scope;
  PaError err;
  PaStreamParameters outputParameters;
  v8::Local<v8::Object> v8Buffer;
  v8::Local<v8::Object> v8Stream;
  PortAudioData* data;
  int sampleRate;
  char str[1000];
  v8::Handle<v8::Value> initArgs[1];
  v8::Handle<v8::Value> toEventEmitterArgs[1];
  v8::Local<v8::Value> v8Val;

  v8::Handle<v8::Value> argv[2];
  argv[0] = v8::Undefined();
  argv[1] = v8::Undefined();

  // options
  if(!args[0]->IsObject()) {
    return scope.Close(v8::ThrowException(v8::Exception::TypeError(v8::String::New("First argument must be an object"))));
  }
  v8::Local<v8::Object> options = args[0]->ToObject();

  // callback
  if(!args[1]->IsFunction()) {
    return scope.Close(v8::ThrowException(v8::Exception::TypeError(v8::String::New("Second argument must be a function"))));
  }
  v8::Local<v8::Value> callback = args[1];

  err = EnsureInitialized();
  if(err != paNoError) {
    sprintf(str, "Could not initialize PortAudio %d", err);
    argv[0] = v8::Exception::TypeError(v8::String::New(str));
    goto openDone;
  }

  if(!g_portAudioStreamInitialized) {
    v8::Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New();
    t->InstanceTemplate()->SetInternalFieldCount(1);
    t->SetClassName(v8::String::NewSymbol("PortAudioStream"));
    streamConstructor = v8::Persistent<v8::Function>::New(t->GetFunction());

    toEventEmitterArgs[0] = streamConstructor;
    v8Val = options->Get(v8::String::New("toEventEmitter"));
    v8::Function::Cast(*v8Val)->Call(options, 1, toEventEmitterArgs);

    g_portAudioStreamInitialized = true;
  }

  memset(&outputParameters, 0, sizeof(PaStreamParameters));

  outputParameters.device = Pa_GetDefaultOutputDevice();
  if (outputParameters.device == paNoDevice) {
    sprintf(str, "No default output device");
    argv[0] = v8::Exception::TypeError(v8::String::New(str));
    goto openDone;
  }

  v8Val = options->Get(v8::String::New("channelCount"));
  outputParameters.channelCount = v8Val->ToInt32()->Value();

  v8Val = options->Get(v8::String::New("sampleFormat"));
  switch(v8Val->ToInt32()->Value()) {
  case 8:
    outputParameters.sampleFormat = paInt8;
    break;
  case 16:
    outputParameters.sampleFormat = paInt16;
    break;
  case 24:
    outputParameters.sampleFormat = paInt24;
    break;
  case 32:
    outputParameters.sampleFormat = paInt32;
    break;
  default:
    argv[0] = v8::Exception::TypeError(v8::String::New("Invalid sampleFormat"));
    goto openDone;
  }
  outputParameters.suggestedLatency = Pa_GetDeviceInfo( outputParameters.device )->defaultLowOutputLatency;
  outputParameters.hostApiSpecificStreamInfo = NULL;

  v8Val = options->Get(v8::String::New("sampleRate"));
  sampleRate = v8Val->ToInt32()->Value();

  data = new PortAudioData();
  data->readIdx = 0;
  data->writeIdx = 1;
  data->channelCount = outputParameters.channelCount;
  data->sampleFormat = outputParameters.sampleFormat;

  v8Stream = streamConstructor->NewInstance();
  v8Stream->SetPointerInInternalField(0, data);
  v8Val = options->Get(v8::String::New("streamInit"));
  initArgs[0] = v8Stream;
  v8::Function::Cast(*v8Val)->Call(v8Stream, 1, initArgs);
  data->v8Stream = v8::Persistent<v8::Object>::New(v8Stream);
  data->v8Stream.MakeWeak(data, CleanupStreamData);
  data->v8Stream.MarkIndependent();

  v8Buffer = v8Stream->Get(v8::String::New("buffer"))->ToObject();
  data->buffer = (unsigned char*)node::Buffer::Data(v8Buffer);
  data->bufferLen = node::Buffer::Length(v8Buffer);

  err = Pa_OpenStream(
    &data->stream,
    NULL, // no input
    &outputParameters,
    sampleRate,
    FRAMES_PER_BUFFER,
    paClipOff, // we won't output out of range samples so don't bother clipping them
    nodePortAudioCallback,
    data);
  if(err != paNoError) {
    sprintf(str, "Could not open stream %d", err);
    argv[0] = v8::Exception::TypeError(v8::String::New(str));
    goto openDone;
  }

  v8Stream->Set(v8::String::New("write"), v8::FunctionTemplate::New(stream_write)->GetFunction());
  v8Stream->Set(v8::String::New("writeByte"), v8::FunctionTemplate::New(stream_writeByte)->GetFunction());
  v8Stream->Set(v8::String::New("start"), v8::FunctionTemplate::New(stream_start)->GetFunction());
  v8Stream->Set(v8::String::New("stop"), v8::FunctionTemplate::New(stream_stop)->GetFunction());

  argv[1] = v8Stream;

openDone:
  v8::Function::Cast(*callback)->Call(v8::Context::GetCurrent()->Global(), 2, argv);
  return scope.Close(v8::Undefined());
} */

NAN_METHOD(GetDevices) {
  // Nan::EscapableHandleScope esc;
  char str[1000];
  int numDevices;
  Nan::Callback *cb = new Nan::Callback(info[0].As<v8::Function>());

  v8::Local<v8::Value> argv[] = { Nan::Null(), Nan::Undefined() };

  PaError err = EnsureInitialized();
  if(err != paNoError) {
    sprintf(str, "Could not initialize PortAudio %d", err);
    argv[0] = Nan::Error(str);
    cb->Call(1, argv);
  } else {
    numDevices = Pa_GetDeviceCount();
    v8::Local<v8::Array> result = Nan::New<v8::Array>(numDevices);
    printf("numDevices %d\n", numDevices);

    for ( int i = 0 ; i < numDevices ; i++) {
      const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(i);
      v8::Local<v8::Object> v8DeviceInfo = Nan::New<v8::Object>();
      Nan::Set(v8DeviceInfo, Nan::New("id").ToLocalChecked(), Nan::New(i));
      Nan::Set(v8DeviceInfo, Nan::New("name").ToLocalChecked(),
        Nan::New(deviceInfo->name).ToLocalChecked());
      Nan::Set(result, i, v8DeviceInfo);
    }

    argv[1] = result;
    cb->Call(2, argv);
  }

  info.GetReturnValue().SetUndefined();
}

/* v8::Handle<v8::Value> GetDevices(const v8::Arguments& args) {
  v8::HandleScope scope;
  char str[1000];
  int numDevices;
  v8::Local<v8::Array> result;

  v8::Handle<v8::Value> argv[2];
  argv[0] = v8::Undefined();
  argv[1] = v8::Undefined();

  // callback
  if(!args[0]->IsFunction()) {
    return scope.Close(v8::ThrowException(v8::Exception::TypeError(v8::String::New("First argument must be a function"))));
  }
  v8::Local<v8::Value> callback = args[0];

  PaError err = EnsureInitialized();
  if(err != paNoError) {
    sprintf(str, "Could not initialize PortAudio %d", err);
    argv[0] = v8::Exception::TypeError(v8::String::New(str));
    goto getDevicesDone;
  }

  numDevices = Pa_GetDeviceCount();
  result = v8::Array::New(numDevices);
  argv[1] = result;
  printf("numDevices %d\n", numDevices);
  for(int i = 0; i < numDevices; i++) {
    const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(i);
    v8::Local<v8::Object> v8DeviceInfo = v8::Object::New();
    v8DeviceInfo->Set(v8::String::New("id"), v8::Integer::New(i));
    v8DeviceInfo->Set(v8::String::New("name"), v8::String::New(deviceInfo->name));
    result->Set(i, v8DeviceInfo);
  }

getDevicesDone:
  v8::Function::Cast(*callback)->Call(v8::Context::GetCurrent()->Global(), 2, argv);
  return scope.Close(v8::Undefined());
}
*/

#define STREAM_DATA \
  PortAudioData* data = (PortAudioData*) Nan::GetInternalFieldPointer(info.This(), 0);

#define EMIT_BUFFER_OVERRUN \
  v8::Local<v8::Value> emitArgs[1]; \
  emitArgs[0] = Nan::New("overrun").ToLocalChecked(); \
  Nan::Call(Nan::Get(info.This(), Nan::New("emit").ToLocalChecked()).ToLocalChecked().As<v8::Function>(), \
    info.This(), 1, emitArgs);

NAN_METHOD(StreamStop) {
  STREAM_DATA;

  if (data != NULL) {
    PaError err = Pa_CloseStream(data->stream);
    if(err != paNoError) {
      char str[1000];
      sprintf(str, "Could not start stream %d", err);
      Nan::ThrowError(str);
    }
  }

  info.GetReturnValue().SetUndefined();
}

NAN_METHOD(StreamStart) {
  STREAM_DATA;

  if (data != NULL) {
    PaError err = Pa_StartStream(data->stream);
    if(err != paNoError) {
      char str[1000];
      sprintf(str, "Could not start stream %s", Pa_GetErrorText(err));
      Nan::ThrowError(str);
    }
  }

  info.GetReturnValue().SetUndefined();
}

/* NAN_METHOD(StreamWriteByte) {
  STREAM_DATA;

  if (data != NULL) {
    if(data->writeIdx == data->readIdx) {
      EMIT_BUFFER_OVERRUN;
    } else {
      int val = Nan::To<int>(info[0]).FromMaybe(0);
      data->buffer[data->writeIdx++] = val;
      if(data->writeIdx >= data->bufferLen) {
        data->writeIdx = 0;
      }
    }
  }

  info.GetReturnValue().SetUndefined();
} */

/* NAN_METHOD(StreamWrite) {
  STREAM_DATA;

  v8::Local<v8::Object> buffer = info[0].As<v8::Object>();
  int bufferLen = node::Buffer::Length(buffer);
  unsigned char* p = (unsigned char*)node::Buffer::Data(buffer);
  for ( int i = 0 ; i < bufferLen ; i++ ) {
    if(data->writeIdx == data->readIdx) {
      EMIT_BUFFER_OVERRUN;
      i = bufferLen;
    }

    data->buffer[data->writeIdx++] = *p++;
    if(data->writeIdx >= data->bufferLen) {
      data->writeIdx = 0;
    }
  }

  info.GetReturnValue().SetUndefined();
} */

void EIO_WritableCallback(uv_work_t* req) {
}

void EIO_WritableCallbackAfter(uv_work_t* req) {
  Nan::HandleScope scope;
  PortAudioData* request = (PortAudioData*)req->data;
  if (request->nextBuffer != NULL) {
    request->buffer = request->nextBuffer;
    request->bufferIdx = request->nextIdx;
    request->protectBuffer.Reset(request->protectNext);
    request->bufferLen = request->nextLen;
  }
  printf("CALLBACK!!! %i\n", request->writeCallback);
  request->writeCallback->Call(0, 0);
}

NAN_METHOD(WritableWrite) {
  STREAM_DATA;
  printf("Writability!!! %i\n", data->bufferLen);

  data->writeCallback = new Nan::Callback(info[2].As<v8::Function>());
  if (data->buffer == NULL) { // Bootstrap
    printf("Bootstrap!\n");
    v8::Local<v8::Object> chunk = info[0].As<v8::Object>();
    int chunkLen = node::Buffer::Length(chunk);
    unsigned char* p = (unsigned char*) node::Buffer::Data(chunk);
    Nan::Callback *writeCallback = new Nan::Callback(info[2].As<v8::Function>());
    data->bufferLen = chunkLen;
    data->buffer = p;
    data->bufferIdx = 0;
    data->protectBuffer.Reset(chunk);
    uv_work_t* req = new uv_work_t();
    req->data = data;
    uv_queue_work(uv_default_loop(), req, EIO_WritableCallback,
      (uv_after_work_cb) EIO_WritableCallbackAfter);
  } else {
    printf("Getting next data.\n");
    v8::Local<v8::Object> chunk = info[0].As<v8::Object>();
    int chunkLen = node::Buffer::Length(chunk);
    unsigned char* p = (unsigned char*) node::Buffer::Data(chunk);
    data->nextLen = chunkLen;
    data->nextBuffer = p;
    data->nextIdx = 0;
    data->protectNext.Reset(chunk);
  }

  info.GetReturnValue().SetUndefined();
  printf("Finished Writability!!!\n");
}

/*void EIO_EmitUnderrun(uv_work_t* req) {

}

void EIO_EmitUnderrunAfter(uv_work_t* req) {
  Nan::HandleScope scope;
  PortAudioData* request = (PortAudioData*)req->data;

  v8::Local<v8::Value> emitArgs[1];
  emitArgs[0] = Nan::New("underrun").ToLocalChecked();
  Nan::Call(Nan::Get(Nan::New(request->v8Stream), Nan::New("emit").ToLocalChecked()).ToLocalChecked().As<v8::Function>(),
    Nan::New(request->v8Stream), 1, emitArgs);
}*/

static int nodePortAudioCallback(
  const void *inputBuffer,
  void *outputBuffer,
  unsigned long framesPerBuffer,
  const PaStreamCallbackTimeInfo* timeInfo,
  PaStreamCallbackFlags statusFlags,
  void *userData) {

  PortAudioData* data = (PortAudioData*)userData;

  unsigned long i;

  int multiplier = 1;
  switch(data->sampleFormat) {
  case paInt8:
    multiplier = 1;
    break;
  case paInt16:
    multiplier = 2;
    break;
  case paInt24:
    multiplier = 3;
    break;
  case paInt32:
    multiplier = 4;
    break;
  }

  multiplier = multiplier * data->channelCount;
  int bytesRequested = ((int) framesPerBuffer) * multiplier;

  unsigned char* out = (unsigned char*) outputBuffer;
  if (data->bufferIdx >= data->bufferLen ) { //read from next
    memcpy(out, data->nextBuffer + data->nextIdx, bytesRequested);
    data->nextIdx += bytesRequested;
  } else if (data->bufferIdx + bytesRequested >= data->bufferLen) {
    uv_work_t* req = new uv_work_t();
    req->data = data;
    uv_queue_work(uv_default_loop(), req, EIO_WritableCallback,
      (uv_after_work_cb) EIO_WritableCallbackAfter);
    int bytesRemaining = data->bufferLen - data->bufferIdx;
    memcpy(out, data->buffer + data->bufferIdx, bytesRemaining);
    memcpy(out + bytesRemaining, data->nextBuffer, bytesRequested - bytesRemaining);
    data->bufferIdx += bytesRequested;
    data->nextIdx = bytesRequested - bytesRemaining;
  } else { // read from buffer
    memcpy(out, data->buffer + data->bufferIdx, bytesRequested);
    data->bufferIdx += framesPerBuffer * multiplier;
  }
  // for(i = 0; i < framesPerBuffer * multiplier; i++) {
  //   if(data->readIdx == data->writeIdx) {
  //     uv_work_t* req = new uv_work_t();
  //     req->data = data;
  //     uv_queue_work(uv_default_loop(), req, EIO_EmitUnderrun, (uv_after_work_cb) EIO_EmitUnderrunAfter);
  //     return paContinue;
  //   }
  //   *out++ = data->buffer[data->readIdx++];
  //   if(data->readIdx >= data->bufferLen) {
  //     data->readIdx = 0;
  //   }
  // }

  return paContinue;
}
