//
// Created by imxqd on 2019-02-25.
//

#ifndef SCRCPY_UDPSERVER_H
#define SCRCPY_UDPSERVER_H


void start_udp_server();

void stop_udp_server(SDL_Thread *thread);
void udp_server_join(SDL_Thread *thread);

int run_udp_server();

#endif //SCRCPY_UDPSERVER_H
