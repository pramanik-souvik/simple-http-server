# Simple HTTP Server (C++)

A lightweight, multi-threaded HTTP server written in Modern C++. This project demonstrates practical concepts in networking, concurrency, file I/O, and object‑oriented design. The server handles **GET requests**, serves **static files**, and uses a **thread pool** for efficient parallel processing.

---

## Features

* **Multi-threaded architecture** using a custom thread pool
* **Handles HTTP GET requests**
* **Serves static files** (HTML, CSS, JS, images, etc.) from a configurable document root
* **Automatic MIME type detection** based on file extension
* **Thread-safe request handling**
* **RAII-based resource management**
* **Cross-platform** (Linux, macOS)

---

## Project Structure

```
simple-http-server.cpp   # Main server implementation
www/                     # Document root (static files served to clients)
```

Place your website files (HTML, CSS, images) inside the `www/` directory.

---

## Build Instructions

### **Requirements**

* C++17 or later
* POSIX sockets (Linux / macOS)
* g++ or clang++

### **Compile**

```bash
g++ -std=c++17 -O2 -pthread simple-http-server.cpp -o simple-http-server
```

---

## Usage

### **Start the Server**

```bash
./simple-http-server
```

The server listens on **port 8080** by default.

### **Access in your browser**

Open:

```
http://localhost:8080
```

Or request specific files:

```
http://localhost:8080/index.html
http://localhost:8080/style.css
http://localhost:8080/image.jpg
```

### **Using curl**

```bash
curl http://localhost:8080
```

---

## Stopping the Server

Press:

```
CTRL + C
```

This cleanly shuts down the application.

---

## How It Works

### **1. Server Initialization**

* Creates a listening socket
* Binds to `0.0.0.0:8080`
* Listens for incoming TCP connections

### **2. Accept Loop**

The main thread:

* Accepts client connections
* Pushes each client socket into the thread pool

### **3. Thread Pool**

* Multiple worker threads wait for tasks
* When a request arrives, a worker processes the client independently

### **4. HTTP Request Parsing**

The server reads:

```
GET /file.txt HTTP/1.1
```

and extracts:

* Method
* Path

### **5. File Serving**

* Maps `/file.txt` to `www/file.txt`
* Determines the MIME type
* Sends HTTP headers + file bytes

---

## Supported MIME Types

```
.html  -> text/html
.css   -> text/css
.js    -> application/javascript
.jpg   -> image/jpeg
.jpeg  -> image/jpeg
.png   -> image/png
.txt   -> text/plain
```

You can easily extend this list in the code.

---

## Future Improvements

You may add:

* Graceful shutdown via signals
* CLI args (e.g., `--port 9000`, `--root static/`)
* Directory listing
* Logging system
* HTTP/1.1 persistent connections
* Routing support

---

## License

This project is released under the MIT License.

---

## Author

Developed as a learning project to demonstrate Modern C++ network and concurrency concepts.
