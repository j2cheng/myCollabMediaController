Step 1: Build everything from project root

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/test/unit_test

This builds:
  build/sources/libmediacontroller.a  — static library (sources + proto generated code)
  build/test/unit_test                — test binary, linked against the library

The library handles proto generation automatically via CMake.

Manual proto generation (for reference only — CMake does this for you):

protoc -I . --cpp_out=. grpc_str_out.proto
protoc -I . \
  --grpc_out=. \
  --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) \
  grpc_str_out.proto

You should get these files:

grpc_str_out.pb.h
grpc_str_out.pb.cc
grpc_str_out.grpc.pb.h
grpc_str_out.grpc.pb.cc

Why this works:

The generated .pb files contain message classes like StatusRequest and HelloReply.
The generated .grpc.pb files contain service base classes, including IntStreamoutSvcStrl::Service from grpc_str_out.proto:7.




g++ -std=c++17 -o hello_fake_test hello_fake_test.cpp \
  grpc_str_out.pb.cc \
  $(pkg-config --cflags --libs protobuf)

--cpp_out → message/data classes (.pb.h / .pb.cc)
--grpc_out → service classes (.grpc.pb.h / .grpc.pb.cc)



-----------only want to do fake testing-------------------------
do: protoc -I . --cpp_out=. grpc_str_out.proto

it will create : grpc_str_out.pb.cc   which is message/data class.


then do: g++ -std=c++17 -o hello_fake_test hello_fake_test.cpp \
  grpc_str_out.pb.cc \
  $(pkg-config --cflags --libs protobuf)


Note: you still need ***protobuf*** here.


grpc_str_out.pb.h contains only message/data classes — no Stub, no Service, no Channel:

StatusRequest, HelloReply, SetPortRequest, SetStreamPipelineRequest, StartRequest, StopRequest, StreamoutResponse, etc.
These all inherit from google::protobuf::Message — they're just data containers for serialization/deserialization.

The gRPC client stub (IntStreamoutSvcStrl::Stub) and service base class (IntStreamoutSvcStrl::Service) would live in grpc_str_out.grpc.pb.h — but that file doesn't even exist on disk anymore (since you only ran --cpp_out, not --grpc_out).

So yes — you're only using protobuf data classes, no gRPC client/server code at all. 
Your fake test works purely at the data layer.

If you compile and link grpc_str_out.grpc.pb.cc, the generated code 
includes <grpcpp/grpcpp.h> and references gRPC runtime symbols — so you must have 
libgrpc++-dev installed, otherwise you'll get missing header / undefined reference errors.

-----------only want to do fake testing-------------------------

--created makeFile-CMakeLists.txt---------
Build from project root:
cmake -S . -B build
cmake --build build
./build/test/unit_test
--created makeFile-CMakeLists.txt---------


----------------------------------------------------------------------
The contract has two sides:

What	Who implements	Where
IMediaControllerInstance	You (your module)	MediaController.cpp
IMediaControllerFactory	You (your module)	MediaController.cpp
MediaControllerManager	mk2 app	Somewhere in the mk2 codebase
The MediaControllerManager singleton is the mk2 app's orchestrator. It:

Calls setFactory() with your factory
Calls createInstance() which uses your factory to make instances
Calls setGlobalCallbacks() to wire up app-level event handling
Calls trigger*() methods to propagate events
You don't need to implement it. When you integrate into mk2, 
the app already has the MediaControllerManager implementation. 
It will use your MediaControllerFactory to create MediaControllerInstance objects.
----------------------------------------------------------------------

cmake -S . -B build && cmake --build build && ./build/hello_fake_test

cd test
cmake -S . -B build
cmake --build build
./build/unit_test


----5-2-2026----------------------------------------------------------
 created new branch, added button to do the test.
 
 git branch --all
  gui_test
* master

---in gui_test branch, do cd test
  cmake -S . -B build
  cmake --build build
  ./build/gui_test

---in master branch, do cd test
  cmake -S . -B build
  cmake --build build
  ./build/unit_test


----------need debug build------------------
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

cd test && cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build


to build only mediacontroller.a:
  cmake --build build --target mediacontroller




------build with Android NDK------------------
cmake -S . -B build-arm64 \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-26 \
  -DMEDIACONTROLLER_INTERFACE_DIR=/path/to/mk2/includes

cmake --build build-arm64 --target mediacontroller



cmake -S . -B build-arm32 \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=armeabi-v7a \
  -DANDROID_PLATFORM=android-26 \
  -DMEDIACONTROLLER_INTERFACE_DIR=/path/to/mk2/includes

cmake --build build-arm32 --target mediacontroller

------build with Android NDK------------------
