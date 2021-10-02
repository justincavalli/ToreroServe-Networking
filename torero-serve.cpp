/**
 * ToreroServe: A Lean Web Server
 * COMP 375 - Project 02
 *
 * This program should take two arguments:
 * 	1. The port number on which to bind and listen for connections
 * 	2. The directory out of which to serve files.
 *
 * Author 1:
 * Author 2: Chadmond Wu, cwu@sandiego.edu
 */

// standard C libraries
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>

// operating system specific libraries
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <pthread.h>

// C++ standard libraries
#include <vector>
#include <thread>
#include <string>
#include <iostream>
#include <system_error>
#include <regex>
#include <filesystem>
#include <fstream>

#include "BoundedBuffer.hpp"

#define BUFFER_SIZE 2048
#define TRANSACTION_CLOSE 2 //size of \r\n


// shorten the std::filesystem namespace down to just fs
namespace fs = std::filesystem;

using std::cout;
using std::string;
using std::vector;
using std::thread;

// This will limit how many clients can be waiting for a connection.
static const int BACKLOG = 10;
const size_t BUFFER_CAPACITY = 10;
const size_t NUM_THREADS = 8;

// forward declarations
int createSocketAndListen(const int port_num);
void acceptConnections(const int server_sock, std::string root);
void handleClient(const int client_sock);
void sendData(int socked_fd, const char *data, size_t data_length);
int receiveData(int socked_fd, char *dest, size_t buff_size);
void consume (BoundedBuffer &buffer, std::string root);

bool validGET(std::string request);
bool fileExists(std::string file_name);
bool isDirectory(std::string file_name);

void sendBad(const int client_sock);
void sendNotFound(const int client_sock);
void sendOK(const int client_sock);

void sendHeader(const int client_sock, std::string file_name);
void sendHTML(const int client_sock, std::string file_name);
void sendFile(const int client_sock, std::string file_name);
void sendError(const int client_sock);

int main(int argc, char** argv) {

	/* Make sure the user called our program correctly. */
	if (argc != 3) {
		cout << "INCORRECT USAGE!\n";
        cout << "Format: './(compiled exec) (port num) (root dir)'\n";
		exit(1);
	}

    /* Read the port number from the first command line argument. */
    int port = std::stoi(argv[1]);

    /* Read the root directory from the second command line argument. */
    std::string root = (argv[2]);

	/* Create a socket and start listening for new connections on the
	 * specified port. */
	int server_sock = createSocketAndListen(port);

	/* Now let's start accepting connections. */
	acceptConnections(server_sock, root);

    close(server_sock);

	return 0;
}

/**
 * Sends message over given socket, raising an exception if there was a problem
 * sending.
 *
 * @param socket_fd The socket to send data over.
 * @param data The data to send.
 * @param data_length Number of bytes of data to send.
 */
void sendData(int socked_fd, const char *data, size_t data_length) {
    while(data_length > 0)
    {
        int num_bytes_sent = send(socked_fd, data, data_length, 0);
        if (num_bytes_sent == -1) {
            std::error_code ec(errno, std::generic_category());
            throw std::system_error(ec, "send failed");
        }
        data_length -= num_bytes_sent; //Readjust send size
        data += num_bytes_sent; //Calculate next buffer position
	}
}

/**
 * Receives message over given socket, raising an exception if there was an
 * error in receiving.
 *
 * @param socket_fd The socket to send data over.
 * @param dest The buffer where we will store the received data.
 * @param buff_size Number of bytes in the buffer.
 * @return The number of bytes received and written to the destination buffer.
 */
int receiveData(int socked_fd, char *dest, size_t buff_size) {
	int num_bytes_received = recv(socked_fd, dest, buff_size, 0);
	if (num_bytes_received == -1) {
		std::error_code ec(errno, std::generic_category());
		throw std::system_error(ec, "recv failed");
	}
	return num_bytes_received;
}

/**
 * Receives a request from a connected HTTP client and sends back the
 * appropriate response.
 *
 * @note After this function returns, client_sock will have been closed (i.e.
 * may not be used again).
 *
 * @param client_sock The client's socket file descriptor.
 */
void handleClient(const int client_sock, std::string root) {
	// Step 1: Receive the request message from the client
	char received_data[BUFFER_SIZE];
	int bytes_received = receiveData(client_sock, received_data, BUFFER_SIZE);

	// Turn the char array into a C++ string for easier processing.
	string request_string(received_data, bytes_received);
    //std::cout << request_string << "\n";

    // Parsing the request string to determine what response to generate.
	// Using regex to determine if a request is properly formatted.
    
	if (!validGET(request_string)) { //Testing for valid request
		sendBad(client_sock);
		return;
	}
	
	std::istringstream f(request_string);
	std::string file_name;
	getline(f, file_name, ' ');
	getline(f, file_name, ' '); //Tokenize file/dir name

    root.append(file_name); //Using root parameter to find directory

	if (!fileExists(root) && !isDirectory(root)) { //Testing for valid file/dir
		sendNotFound(client_sock); //Sending 404 if not found
        sendError(client_sock); //If non-existent file/dir, don't send header
		return;
	}

    // Generate HTTP response message based on the request you received.
    // Sends response to client using sendData.

    //Response is split into two sections: headers and relevant content
    //to avoid any data width conflicts. 

	sendOK(client_sock);

    if (isDirectory(root))
    {
        sendHTML(client_sock, root);
    }

	else if (fileExists(root)) { //If not directory, send file immediately
		sendHeader(client_sock, root);
		sendFile(client_sock, root);
    }
	close(client_sock);
}

/**
 * Creates a new socket and starts listening on that socket for new
 * connections.
 *
 * @param port_num The port number on which to listen for connections.
 * @returns The socket file descriptor
 */
int createSocketAndListen(const int port_num) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Creating socket failed");
        exit(1);
    }

    /* 
	 * A server socket is bound to a port, which it will listen on for incoming
     * connections.  By default, when a bound socket is closed, the OS waits a
     * couple of minutes before allowing the port to be re-used.  This is
     * inconvenient when you're developing an application, since it means that
     * you have to wait a minute or two after you run to try things again, so
     * we can disable the wait time by setting a socket option called
     * SO_REUSEADDR, which tells the OS that we want to be able to immediately
     * re-bind to that same port. See the socket(7) man page ("man 7 socket")
     * and setsockopt(2) pages for more details about socket options.
	 */
    int reuse_true = 1;

	int retval; // for checking return values

    retval = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse_true,
                        sizeof(reuse_true));

    if (retval < 0) {
        perror("Setting socket option failed");
        exit(1);
    }

    /*
	 * Create an address structure.  This is very similar to what we saw on the
     * client side, only this time, we're not telling the OS where to connect,
     * we're telling it to bind to a particular address and port to receive
     * incoming connections.  Like the client side, we must use htons() to put
     * the port number in network byte order.  When specifying the IP address,
     * we use a special constant, INADDR_ANY, which tells the OS to bind to all
     * of the system's addresses.  If your machine has multiple network
     * interfaces, and you only wanted to accept connections from one of them,
     * you could supply the address of the interface you wanted to use here.
	 */
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_num);
    addr.sin_addr.s_addr = INADDR_ANY;

    /* 
	 * As its name implies, this system call asks the OS to bind the socket to
     * address and port specified above.
	 */
    retval = bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    if (retval < 0) {
        perror("Error binding to port");
        exit(1);
    }

    /* 
	 * Now that we've bound to an address and port, we tell the OS that we're
     * ready to start listening for client connections. This effectively
	 * activates the server socket. BACKLOG (a global constant defined above)
	 * tells the OS how much space to reserve for incoming connections that have
	 * not yet been accepted.
	 */
    retval = listen(sock, BACKLOG);
    if (retval < 0) {
        perror("Error listening for connections");
        exit(1);
    }
	return sock;
}

/**
 * Sit around forever accepting new connections from client.
 *
 * @param server_sock The socket used by the server.
 */
void acceptConnections(const int server_sock, std::string root) {
    
    BoundedBuffer buff(BUFFER_CAPACITY);

    for (size_t i = 0; i < NUM_THREADS; ++i) {
		std::thread consumer(consume, std::ref(buff), root); //create 8 threads, waiting on shared buffer

		// let the consumers run without us waiting to join with them
		consumer.detach();
	}

    while (true) {
        // Declare a socket for the client connection.
        int sock;

        /* 
		 * Another address structure.  This time, the system will automatically
         * fill it in, when we accept a connection, to tell us where the
         * connection came from.
		 */
        struct sockaddr_in remote_addr;
        unsigned int socklen = sizeof(remote_addr); 

        /* 
		 * Accept the first waiting connection from the server socket and
         * populate the address information.  The result (sock) is a socket
         * descriptor for the conversation with the newly connected client.  If
         * there are no pending connections in the back log, this function will
         * block indefinitely while waiting for a client connection to be made.
         */
        sock = accept(server_sock, (struct sockaddr*) &remote_addr, &socklen);
        if (sock < 0) {
            perror("Error accepting connection");
            exit(1);
        }

        /* 
		 * At this point, you have a connected socket (named sock) that you can
         * use to send() and recv(). The handleClient function should handle all
		 * of the sending and receiving to/from the client.
		 *
		 * handleClient is called from a separate thread by putting sock
		 * into a shared buffer that is synchronized using condition variables.
         * 
         * Producer puts sock into BoundedBuffer, consumer threads take out.
		 */

		buff.putItem(sock);
    }
}
/**
 * Allows threads to wait on shared buffer to have a client socket available.
 * Calls handleClient when available. Producer puts sock into BoundedBuffer, 
 * consumer threads take out.
 *
 * @param buffer A bounded buffer, shared amongst several threads.
 * @param root The directory root name (ex. WWW/test/)
 */
void consume (BoundedBuffer &buffer, std::string root) {
    while (true) {
        int shared_sock = buffer.getItem(); //thread gets socket from shared buffer
        handleClient(shared_sock, root); //when available
    }

}

/**
 * Checks for a valid HTTP GET request message.
 *
 * @param request Given request message from client.
 * @returns true if the GET request is valid
 */

bool validGET(std::string request) {
    //checks for GET \s (whitespace) \w ./(filename and extension) HTTP \d.\d (any HTTP version)
    std::regex http_request_regex("(GET\\s[\\w\\-\\./]*\\sHTTP/\\d\\.\\d)");
	std::smatch request_match;

	if (std::regex_search(request, request_match, http_request_regex)) {
        return true;
    }
    else {
        return false;
    }
}
/**
 * Checks if the requested file exists and is in the root directory.
 *
 * @param file_name The requested file.
 * @returns true if the file exists with no errors in the stream
 */

bool fileExists(std::string file_name) {
    std::ifstream f(file_name.c_str()); //input file stream object
    return f.good(); //true if no errors in stream

    //fs::path name(file_name);
    //return fs::exists(name);
}

/**
 * Checks if the requested directory exists and is in the root directory.
 *
 * @param file_name The requested file (a directory in this case).
 * @returns true if the filename ends with '/'
 */

bool isDirectory(std::string file_name) {
    //return (file_name.back() == '/');
    return (fs::is_directory(file_name)); //CW
}

/**
 * Sends a HTTP 400 BAD REQUEST response.
 *
 * @param client_sock The client's socket file descriptor.
 */

void sendBad(const int client_sock) {
    std::string rs = "HTTP/1.0 400 BAD REQUEST\r\n";
    sendData(client_sock, rs.c_str(), rs.length()); //Send response first, headers/data in seperate functions
}

/**
 * Sends a HTTP 404 NOT FOUND response.
 *
 * @param client_sock The client's socket file descriptor.
 */

void sendNotFound(const int client_sock) {
    std::string rs = "HTTP/1.0 404 NOT FOUND\r\n";
    sendData(client_sock, rs.c_str(), rs.length());
}

/**
 * Sends a HTTP 200 OK response.
 *
 * @param client_sock The client's socket file descriptor.
 */

void sendOK(const int client_sock) {
    std::string rs = "HTTP/1.0 200 OK\r\n";
    sendData(client_sock, rs.c_str(), rs.length());
}

/**
 * Sends the relevant HTTP headers, excluding file data.
 * Uses stringstream to facilitate future expansion (only
 * sends the bare minimum of specifications).
 *
 * @param client_sock The client's socket file descriptor.
 * @param file_name The requested file to send. 
 */

void sendHeader(const int client_sock, std::string file_name) {
    std::regex rgx("(\\.\\w*)"); //regex expression matches '\. \w*', checking for file extension
    std::smatch request_match;
    std::string file_type;
    std::string size;

    if (std::regex_search(file_name, request_match, rgx))
    {
        //Most common file types, more types can be added here if needed
        if (request_match[0] == ".html")
        {
            file_type = "text/html";
        }
        else if (request_match[0] == ".css")
        {
            file_type = "text/css";
        }
        else if (request_match[0] == ".jpg")
        {
            file_type = "image/jpeg";
        }
        else if (request_match[0] == ".gif")
        {
            file_type = "image/gif";
        }
        else if (request_match[0] == ".png")
        {
            file_type = "image/png";
        }
        else if (request_match[0] == ".pdf") //Firefox error?
        {
            file_type = "application/pdf";
        }
        else {
            file_type = "text/plain"; 
        }
    }
    else {
        std::cout << "No match!\n";
        return;
    }

    std::stringstream ss;
    ss  << "Content-Type: " << file_type << "\r\n"
        << "Content-Length: " << std::to_string(fs::file_size(file_name)) << "\r\n"
        << "\r\n";

    std::string response = ss.str();
    sendData(client_sock, response.c_str(), response.length());
}

/**
 * Generates an HTML file that lists the files or other directories 
 * inside a specified directory. Checks if the specified dir has 
 * index.html; if so, it displays index.html instead.
 *
 * @param client_sock The client's socket file descriptor.
 * @param file_name The requested file to send. 
 */
  
void sendHTML(const int client_sock, std::string file_name) {
	 // handle client should check if URL ends in a /

     std::stringstream ss;

     if (!isDirectory(file_name) && fileExists(file_name)) { //If file/dir not found, throw generic error page (not a 400/404 error)
     	ss  << "<html>" << "\r\n"
            << "<head>" << "\r\n"
            << "<title> Page not found! </title>" << "\r\n"
            << "</head>" << "\r\n"
            << "<body> 404 Page Not Found! </body>" << "\r\n"
            << "</html>" << "\r\n";
			
			std::string error_pg = ss.str();
			std::stringstream response;
			response << "Content-Type: " << "text/html" << "\r\n"
					 << "Content-Length: " << error_pg.length() << "\r\n"
					 << "\r\n" << error_pg << "\r\n";
			
            std::string send_pg1 = response.str();	
            sendData(client_sock, send_pg1.c_str(), send_pg1.length());
            return;
     }

    //Start auto-generating HTML directory list page
     ss  << "<html>" << "\r\n"
         << "<head>" << "<title></title>" << "</head>" << "\r\n" //Can modify for better UX
         << "<body>" << "\r\n"
         << "<ul>" << "\r\n"; //Making auto-generated bulleted list

     for (auto& entry: fs::directory_iterator(file_name)) {
        //std::string file = entry.path().filename(); 

     	if (entry.path().filename() == "index.html") { //If we find index.html, stop auto-generating HTML and return the index immediately
            sendHeader(client_sock, entry.path());
            sendFile(client_sock, entry.path());
            return;
        }
        else if (fs::is_regular_file(file_name + entry.path().filename().string())) { //Check for filenames and add in all files
	    	ss << "\t<li><a href=\"" << entry.path().filename().string() << "\">" << entry.path().filename().string() << "</a></li>\r\n";
        }
		else if (fs::is_directory(file_name + entry.path().filename().string())) {
			ss << "\t<li><a href=\"" << entry.path().filename().string() << "/\">" << entry.path().filename().string() << "/</a></li>\r\n"; //generate HTML href
        }
        else {
            //cout << "Blank file directory\r\n"; //Given directory is empty
        }
   	 }
    	ss << "</ul>" << "\r\n"
	   	   << "</body>" << "\r\n"
    	   << "</html>" << "\r\n";
	 
	 std::string html_pg = ss.str();
	 std::stringstream response2;

	 response2  << "Content-Type: " << "text/html" << "\r\n" //appending header info
	 			<< "Content-Length: " << html_pg.length() << "\r\n"
				<< "\r\n" << html_pg << "\r\n";
	 std::string send_pg2 = response2.str();
	 sendData(client_sock, send_pg2.c_str(), send_pg2.length());
}
/**
 * Sends the requested file, separate from the headers.
 * 
 * @param client_sock The client's socket file descriptor.
 * @param file_name The requested file to send. 
 */

void sendFile(const int client_sock, std::string file_name) {
    //open file, place data into buffer, and send
    std::ifstream file(file_name, std::ios::binary);

    const unsigned int buffer_size = 4096;
    char file_data[buffer_size];

    //keep reading until EOF
    while (!file.eof()) {
        file.read(file_data, buffer_size); //Read up to buffer_size bytes into data buffer
        int bytes_read = file.gcount();
        sendData(client_sock, file_data, bytes_read);
    }
    file.close();
    sendData(client_sock, "\r\n", TRANSACTION_CLOSE); //close transaction
}

void sendError(const int client_sock) {

    cout << "Error!";
    std::stringstream ss;
    ss  << "<html>" << "\r\n"
    << "<head>" << "\r\n"
    << "<title> Page not found! </title>" << "\r\n"
    << "</head>" << "\r\n"
    << "<body> 404 Page Not Found! </body>" << "\r\n"
    << "</html>" << "\r\n";
    
    std::string error_pg = ss.str();
    std::stringstream response;
    response << "Content-Type: " << "text/html" << "\r\n"
                << "Content-Length: " << error_pg.length() << "\r\n"
                << "\r\n" << error_pg << "\r\n";
    
    std::string send_pg1 = response.str();	
    sendData(client_sock, send_pg1.c_str(), send_pg1.length());
    return;
}
