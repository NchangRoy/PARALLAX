#include"network_agent.h"
#include"socket.h"
#include"ms_queue.h"
#include<pthread.h>

map_registry * registry;

void * socket_listener(void * args){
    connection * local_connection=(connection *)args;
    /*
        1. listen for new message on the socket
        2. when it receives a new message , it deserialize it into message_t
        3. from the type of the message it get the message queue it it to write the data into using hte find_by_msg_type function
        4. it then write the data into that message queue
    */
    

}
void * socket_sender(void * args){
    /*
    this is continously checking for message in the message queue,
    once it gets a new message
    it creates a socket connection to the target and sends the message
    
    */
}
void start(){
    pthread_t listener;
    pthread_t sender;
    //create listening socket for the local machine
    connection * local_connection=create_listener("127.0.0.1",9000,1);

    //create recieving message queue to store outgoing messages
    create_mq("outgoing",NULL);
    
    map_entry * outgoing_mq=find_by_msg_type("outgoing");




    //start thread to listen for incoming messages
    pthread_create(&listener,NULL,socket_listener,(void *)local_connection);

    //start thread to listen for messages in outgoing mq and send them
    pthread_create(&sender,NULL,socket_sender,(void * )outgoing_mq);

    pthread_join(listener,NULL);
    pthread_join(sender,NULL);


}


void stop(){

}


void send_msg(char * Ip, int port, message_t*  message ){
    /*
        this function is to send the message to the outgoing message queue
        //it should get the map_entry of the outgoing message using the find_by_msg_type
        then write the message_t into the message queue
    */
}