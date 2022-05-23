janken
====

An online [RPS](https://en.wikipedia.org/wiki/Rock_paper_scissors)/[じゃんけん](https://ja.wikipedia.org/wiki/じゃんけん)/[石头剪子布](https://zh.wikipedia.org/wiki/石头、剪子、布) game.

## Build Instructions

You need CMake and ncurses.

```console
$ mkdir build
$ cd build
$ cmake ..
$ make
```

The server and client program will be located at `bin/server` and `bin/client`, respectively.

## Usage

By default, the server listens at `0.0.0.0:22502`, and the client connects to `127.0.0.1:22502`.

You can change the IP/port to listen/connect with command-line arguments: see `--help`.

In the client, first enter your nickname and press <kbd>Enter</kbd> to login. Then use <kbd>Tab</kbd>, arrow keys and <kbd>Enter</kbd> to select and perform actions.

## Screenshots

![image](https://user-images.githubusercontent.com/47456195/169741213-7d26640e-b9ad-4318-ac36-d3127c9cf653.png)

![image](https://user-images.githubusercontent.com/47456195/169743474-c5bf1ca4-cf19-43da-b08f-8da8b65769f9.png)
