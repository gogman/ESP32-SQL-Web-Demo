/*
 * ChunkServer - the Arduino WebServer with an efficient way to send
 * "chunked" responses.
 *
 * Background: when a handler does not know the length of its response in
 * advance (a query result streamed row by row, for example), HTTP/1.1 lets
 * it send the body as a series of chunks. Each chunk on the wire looks like
 *
 *     <length in hexadecimal>\r\n
 *     <the data>\r\n
 *
 * and a chunk of length zero ends the response.
 *
 * WebServer::sendContent() produces exactly that, but with three separate
 * writes to the network connection: the length line, the data, and the
 * closing line break. The server disables Nagle's algorithm, so each write
 * leaves immediately as its own TCP packet. A response of 100 chunks thus
 * costs about 300 packets. Measured on the board, that made sending slower
 * than the database query itself.
 *
 * ChunkServer::sendChunk() assembles the three parts in one buffer and
 * writes them once: one packet per chunk.
 *
 * The class is header-only because it is small and used from two places.
 */

#ifndef CHUNK_SERVER_H
#define CHUNK_SERVER_H

#include <WebServer.h>

class ChunkServer : public WebServer
{
  /*
   * Extra bytes a chunk needs around its data: the hexadecimal length (at
   * most 8 digits for a 32-bit size), "\r\n" after it, and "\r\n" after the
   * data. 12 bytes cover all of that.
   */
  static const size_t FRAME_OVERHEAD = 12;

public:
  /*
   * The largest amount of data sendChunk() puts into a single write.
   *
   * A TCP packet on a typical WiFi network carries up to about 1436 bytes
   * of payload. 1400 bytes of data plus the few bytes of framing stay below
   * that, so a full chunk still travels as exactly one packet.
   */
  static const size_t MAX_CHUNK_SIZE = 1400;

  /* Reuse the base class constructors, e.g. ChunkServer server(80). */
  using WebServer::WebServer;

  /*
   * Sends one chunk of a response that was started with
   * setContentLength(CONTENT_LENGTH_UNKNOWN) followed by send().
   *
   * data does not need to be null-terminated; exactly length bytes are sent.
   */
  void sendChunk(const char *data, size_t length)
  {
    /*
     * The buffer the chunk is assembled in. It is static so it lives in
     * permanent memory instead of on the stack: handlers already use a fair
     * amount of stack, and requests are served one at a time, so a single
     * shared buffer is safe.
     */
    static char frame[MAX_CHUNK_SIZE + FRAME_OVERHEAD];

    /*
     * In chunked encoding a zero-length chunk means "end of response".
     * Sending one by accident would cut the response short, so empty data
     * is simply ignored. Use endChunks() to end the response on purpose.
     */
    if (length == 0)
      return;

    /*
     * Two cases are handed to the base class instead:
     *
     * - _chunked is false when the browser spoke HTTP/1.0, which has no
     *   chunked encoding. The base class then sends the data unframed.
     * - Data larger than the buffer cannot be framed here. The base class
     *   handles it correctly, only with its three writes.
     */
    if (!_chunked || length > MAX_CHUNK_SIZE)
    {
      sendContent(data, length);
      return;
    }

    /*
     * Build "<hex length>\r\n<data>\r\n" in the buffer. snprintf returns
     * the number of characters in the length line, which is where the data
     * has to start.
     */
    int header = snprintf(frame, FRAME_OVERHEAD, "%x\r\n", (unsigned)length);
    memcpy(frame + header, data, length);
    memcpy(frame + header + length, "\r\n", 2);

    /* One write, one packet. */
    _currentClient.write((const uint8_t *)frame, header + length + 2);
  }

  /*
   * Sends a string constant as one chunk, for example sendChunk("</table>").
   *
   * The template captures the size of the character array at compile time
   * (N includes the terminating zero, hence N - 1), so no strlen() call is
   * needed at run time. It only accepts real arrays; for a pointer, use the
   * two-argument version above.
   */
  template <size_t N>
  void sendChunk(const char (&text)[N])
  {
    sendChunk(text, N - 1);
  }

  /*
   * Ends a chunked response by sending the zero-length chunk. The base
   * class does this when sendContent() is called with a length of 0, and it
   * also resets its own "chunked" state for the next request.
   */
  void endChunks()
  {
    sendContent("", 0);
  }
};

#endif
