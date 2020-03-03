/* This file will contain your solution. Modify it as you wish. */
#include <types.h>
#include <lib.h>
#include <synch.h>
#include "producerconsumer_driver.h"

/* Declare any variables you need here to keep track of and
   synchronise your bounded. A sample declaration of a buffer is shown
   below. It is an array of pointers to items.

   You can change this if you choose another implementation.
   However, you should not have a buffer bigger than BUFFER_SIZE
*/

// data_item_t * item_buffer[BUFFER_SIZE];

/* 
 * define structure for keep tracking the buf
 * status and locks status
 */
typedef struct{
   data_item_t * item_buffer[BUFFER_SIZE];   /* buffer for holding the resources */
   int size;                                 /* for tracking the size of the buffer */
   int front;                                /* pointed to the first element of the buf */
   int rear;                                 /* pointed to the last element to the buf */
   struct semaphore * empty;                 /* keep tracking the empty slots, init with size */
   struct semaphore * full;                  /* keep tracking the items in the buff init with 0 */
   struct semaphore * mutex;                 /* mutex lock for entering the CR */
}buf_state_t;

buf_state_t * my_small_buf;

/* consumer_receive() is called by a consumer to request more data. It
   should block on a sync primitive if no data is available in your
   buffer. It should not busy wait! */

data_item_t * consumer_receive(void)
{
        data_item_t * item;


        /*****************
         * Remove everything just below when you start.
         * The following code is just to trick the compiler to compile
         * the incomplete initial code
         ****************/

        /* NEVER HOLD THE LOCK AND GO TO SLEEP! */
        P(my_small_buf->full);
        P(my_small_buf->mutex);

        /* critical region */
        
        /* 
         * WHEN BLOCKED FRONT IS 0 BUT WHEN WAKE UP REAR = 1 
         * SO MUST PRE INCREASE FRONT BY 1
         */
        item = my_small_buf->
               item_buffer[(++my_small_buf->front) % BUFFER_SIZE];
        /* critical region */
        
        V(my_small_buf->mutex);
        V(my_small_buf->empty);

        /******************
         * Remove above here
         */

        return item;
}

/* procucer_send() is called by a producer to store data in your
   bounded buffer.  It should block on a sync primitive if no space is
   available in your buffer. It should not busy wait!*/

void producer_send(data_item_t *item)
{
        /* NEVER HOLD THE LOCK AND GO TO SLEEP! */
        P(my_small_buf->empty);
        P(my_small_buf->mutex);

        /* critical region */
        
        /* 
         * TO BE BLOCKED WHEN BUF IS FULL==>empty is 0, 
         * ==> REAR = FRONT BUT WHEN WAKE UP FRONT WILL MOVE ONE
         * BLOCK FORWARD SO MUST PRE INCREASE REAR BY 1
         * OTHERWISE WILL OVERWRITE THE CURRENT BLOCK! 
         */
        my_small_buf->
                     item_buffer[(++my_small_buf->rear) % BUFFER_SIZE]
                     = item;

        /* critical region */
        
        V(my_small_buf->mutex);
        V(my_small_buf->full);
}




/* Perform any initialisation (e.g. of global data) you need
   here. Note: You can panic if any allocation fails during setup */

void producerconsumer_startup(void){
   
   /* 
    * initializing everything we need 
    * +initial buff to all NULL pointers
    * +initial front=rear=0
    * +initial size = BUFSIZE
    * +initial semaphore empty = BUFSIZE
    * +initial semaphore full  = 0
    * +initial semophore mutex = 1
    */

   my_small_buf = (buf_state_t *)kmalloc(sizeof(buf_state_t));
   if (my_small_buf == NULL){
      panic("Can't allocate memory!\n");
   }


   for (int i = 0; i < BUFFER_SIZE; i++){
      my_small_buf->item_buffer[i] = NULL;
   }
   
   my_small_buf->size = BUFFER_SIZE;
   
   my_small_buf->front = my_small_buf->rear = 0;
   
   my_small_buf->empty = sem_create("empty",BUFFER_SIZE);
   if(!my_small_buf->empty) {
      panic("producerconsumer: couldn't create semaphore\n");
   }
   
   my_small_buf->full = sem_create("full",0);
   if(!my_small_buf->full) {
      panic("producerconsumer: couldn't create semaphore\n");
   }
   
   my_small_buf->mutex = sem_create("mutex",1);
   if(!my_small_buf->mutex) {
      panic("producerconsumer: couldn't create semaphore\n");
   }
}

/* Perform any clean-up you need here */
void producerconsumer_shutdown(void){
   
   // /* clean up the semaphores */
   sem_destroy(my_small_buf->empty);

   sem_destroy(my_small_buf->full);

   sem_destroy(my_small_buf->mutex);

   kfree(my_small_buf);

}
