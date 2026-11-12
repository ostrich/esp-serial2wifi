#ifndef API_TEST_ESPCONN
#define API_TEST_ESPCONN
struct espconn { int placeholder; void *reverse; };
int espconn_disconnect(struct espconn *connection);
#endif
