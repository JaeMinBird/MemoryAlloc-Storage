#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <err.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include "net.h"
#include "jbod.h"

/* the client socket descriptor for the connection to the server */
int cli_sd = -1;

/* attempts to read n (len) bytes from fd; returns true on success and false on failure.
It may need to call the system call "read" multiple times to reach the given size len.
*/
static bool nread(int fd, int len, uint8_t *buf) {
    int total_read = 0;
    while (total_read < len) {
        int bytes_read = read(fd, buf + total_read, len - total_read);
        if (bytes_read < 0) {
            if (errno == EINTR)
                continue; // ignore interrupts
            return false;
        } else if (bytes_read == 0) {
            break; // EOF
        }
        total_read += bytes_read;
    }
    return total_read == len;
}

/* attempts to write n bytes to fd; returns true on success and false on failure
It may need to call the system call "write" multiple times to reach the size len.
*/
static bool nwrite(int fd, int len, uint8_t *buf) {
    int total_written = 0;
    while (total_written < len) {
        int bytes_written = write(fd, buf + total_written, len - total_written);
        if (bytes_written < 0) {
            if (errno == EINTR)
                continue;
            return false; // write error
        }
        total_written += bytes_written;
    }
    return total_written == len;
}

/* Through this function call the client attempts to receive a packet from sd
(i.e., receiving a response from the server.). It happens after the client previously
forwarded a jbod operation call via a request message to the server.
It returns true on success and false on failure.
The values of the parameters (including op, ret, block) will be returned to the caller of this function:

op - the address to store the jbod "opcode"
ret - the address to store the info code (lowest bit represents the return value of the server side calling the corresponding jbod_operation function. 2nd lowest bit represent whether data block exists after HEADER_LEN.)
block - holds the received block content if existing (e.g., when the op command is JBOD_READ_BLOCK)

In your implementation, you can read the packet header first (i.e., read HEADER_LEN bytes first),
and then use the length field in the header to determine whether it is needed to read
a block of data from the server. You may use the above nread function here.
*/
static bool recv_packet(int sd, uint32_t *op, uint8_t *ret, uint8_t *block) {
    uint8_t header[HEADER_LEN];
    if (!nread(sd, HEADER_LEN, header))
        return false;

    uint32_t net_op;
    memcpy(&net_op, header, sizeof(uint32_t));
    *op = ntohl(net_op);    // imp! convert opcode from network byte order to host order
    *ret = (header[sizeof(uint32_t)]);

    if (*ret & 0x02) { // check if data block is present
        return nread(sd, JBOD_BLOCK_SIZE, block);
    }
    return true;
}


/* The client attempts to send a jbod request packet to sd (i.e., the server socket here);
returns true on success and false on failure.

op - the opcode.
block- when the command is JBOD_WRITE_BLOCK, the block will contain data to write to the server jbod system;
otherwise it is NULL.

The above information (when applicable) has to be wrapped into a jbod request packet (format specified in readme).
You may call the above nwrite function to do the actual sending.
*/
static bool send_packet(int sd, uint32_t op, uint8_t *block) {
    uint8_t buffer[HEADER_LEN + JBOD_BLOCK_SIZE];
    uint32_t net_op = htonl(op); // Convert opcode to network byte order
    memcpy(buffer, &net_op, sizeof(uint32_t)); // Place opcode in buffer

    buffer[4] = 0;

    if (block != NULL && (op & 0x3F) == JBOD_WRITE_BLOCK) {
        buffer[4] |= 0x02;
        memcpy(buffer + HEADER_LEN, block, JBOD_BLOCK_SIZE);
    }

    // send the packet
    int totalLength = HEADER_LEN + ((block != NULL && (op & 0x3F) == JBOD_WRITE_BLOCK) ? JBOD_BLOCK_SIZE : 0);
    return nwrite(sd, totalLength, buffer);
}

/* attempts to connect to server and set the global cli_sd variable to the
 * socket; returns true if successful and false if not.
 * this function will be invoked by tester to connect to the server at given ip and port.
 * you will not call it in mdadm.c
*/
bool jbod_connect(const char *ip, uint16_t port) {
    struct sockaddr_in server_addr;
    cli_sd = socket(AF_INET, SOCK_STREAM, 0);
    if (cli_sd < 0)
        return false; // socket creation failed

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0)
        return false; // address conversion failed

    if (connect(cli_sd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(cli_sd);
        return false; // connection failed
    }

    return true; // successful
}

/* disconnects from the server and resets cli_sd */
void jbod_disconnect(void) {
    if (cli_sd != -1) {
        close(cli_sd);
        cli_sd = -1;
    }
}

/* sends the JBOD operation to the server (use the send_packet function) and receives
(use the recv_packet function) and processes the response.

The meaning of each parameter is the same as in the original jbod_operation function.
return: 0 means success, -1 means failure.
*/
int jbod_client_operation(uint32_t op, uint8_t *block) {
    fflush(stdout);
    if (cli_sd == -1) {
        fprintf(stderr, "Error: No connection to JBOD server.\n");
        return -1; // not connected
    }

    // send the JBOD request packet to the server
    if (!send_packet(cli_sd, op, block)) {
        fprintf(stderr, "Error: Failed to send JBOD request to server.\n");
        return -1; // failed to send packet
    }

    uint32_t received_op;
    uint8_t ret;
    uint8_t temp_block[JBOD_BLOCK_SIZE];

    // receive a response packet from the server
    if (!recv_packet(cli_sd, &received_op, &ret, temp_block)) {
        fprintf(stderr, "Error: Failed to receive JBOD response from server.\n");
        return -1; // Failed to receive packet
    }

    // check if a block of data was received and copy it if necessary
    if ((ret & 0x02) && block != NULL) {
        memcpy(block, temp_block, JBOD_BLOCK_SIZE);
    }

    return (ret & 0x01) ? -1 : 0;
}

