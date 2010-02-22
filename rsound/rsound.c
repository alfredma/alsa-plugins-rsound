#include "rsound.h"

#define DEBUG(x) fprintf(stderr, x);

int rsnd_is_little_endian(void)
{
	uint16_t i = 1;
	return *((uint8_t*)&i);
}

void rsnd_swap_endian_16 ( uint16_t * x )
{
	*x = (*x>>8) | (*x<<8);
}

void rsnd_swap_endian_32 ( uint32_t * x )
{
	*x = 	(*x >> 24 ) |
			((*x<<8) & 0x00FF0000) |
			((*x>>8) & 0x0000FF00) |
			(*x << 24);
}

int rsnd_connect_server( snd_pcm_rsound_t *rd )
{
	struct addrinfo hints, *res;
	memset(&hints, 0, sizeof( hints ));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
   
   getaddrinfo(rd->host, rd->port, &hints, &res);

	rd->conn.socket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
   rd->conn.ctl_socket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

	if ( connect(rd->conn.socket, res->ai_addr, res->ai_addrlen) != 0 )
		return 0;

	if ( connect(rd->conn.ctl_socket, res->ai_addr, res->ai_addrlen) != 0 )
		return 0;

   if ( fcntl(rd->conn.socket, F_SETFL, O_NONBLOCK) < 0)
   {
      fprintf(stderr, "Couldn't set socket to non-blocking ...\n");
      return 0;
   }

	freeaddrinfo(res);
	return 1;
}

int rsnd_send_header_info(snd_pcm_rsound_t *rd)
{
#define HEADER_SIZE 44
	char buffer[HEADER_SIZE] = {0};
	int rc = 0;

#define RATE 24
#define CHANNEL 22
#define FRAMESIZE 34

	uint32_t sample_rate_temp = rd->rate;
	uint16_t channels_temp = rd->channels;
	uint16_t framesize_temp = 16;

	if ( !rsnd_is_little_endian() )
	{
		rsnd_swap_endian_32(&sample_rate_temp);
		rsnd_swap_endian_16(&channels_temp);
		rsnd_swap_endian_16(&framesize_temp);
	}

	*((uint32_t*)(buffer+RATE)) = sample_rate_temp;
	*((uint16_t*)(buffer+CHANNEL)) = channels_temp;
	*((uint16_t*)(buffer+FRAMESIZE)) = framesize_temp;

   struct pollfd fd;
   fd.fd = rd->conn.socket;
   fd.events = POLLOUT;

   if ( poll(&fd, 1, 500) < 0 )
   {
		close(rd->conn.socket);
		close(rd->conn.ctl_socket);
      return 0;
   }

   if (fd.revents & POLLHUP )
   {
		close(rd->conn.socket);
		close(rd->conn.ctl_socket);
      return 0;
   }

	rc = send ( rd->conn.socket, buffer, HEADER_SIZE, 0);
	if ( rc != HEADER_SIZE )
	{
		close(rd->conn.socket);
		close(rd->conn.ctl_socket);
		return 0;
	}

	return 1;
}

int rsnd_get_backend_info ( snd_pcm_rsound_t *rd )
{
	uint32_t chunk_size_temp;
	int rc;

   struct pollfd fd;
   fd.fd = rd->conn.socket;
   fd.events = POLLIN;

   if ( poll(&fd, 1, 500) < 0 )
   {
      close(rd->conn.socket);
      close(rd->conn.ctl_socket);
      return 0;
   }

   if ( fd.revents & POLLHUP )
   {
      close(rd->conn.socket);
      close(rd->conn.ctl_socket);
      return 0;
   }

	rc = recv(rd->conn.socket, &chunk_size_temp, sizeof(uint32_t), 0);
	if ( rc != sizeof(uint32_t))
	{
		close(rd->conn.socket);
      close(rd->conn.ctl_socket);
		return 0;
	}

	chunk_size_temp = ntohl(chunk_size_temp);

   rd->chunk_size = chunk_size_temp;

	rd->buffer = realloc ( rd->buffer, rd->buffer_size );
	rd->buffer_pointer = 0;

	return 1;
}
int rsnd_create_connection(snd_pcm_rsound_t *rd)
{
	int rc;

   if ( rd->conn.socket < 0 && rd->conn.ctl_socket < 0 )
   {
      rc = rsnd_connect_server(rd);
      rd->io.poll_fd = rd->conn.socket;
      snd_pcm_ioplug_reinit_status(&rd->io);
      if (!rc)
      {
         close(rd->conn.socket);
         close(rd->conn.ctl_socket);
         rd->conn.socket = -1;
         rd->conn.ctl_socket = -1;
         return 0;
      }
   }
   if ( !rd->ready_for_data )
   {
      rc = rsnd_send_header_info(rd);
      if (!rc)
      {
         rsound_stop(&rd->io);
         return 0;
      }

      rc = rsnd_get_backend_info(rd);
      if (!rc)
      {
         rsound_stop(&rd->io);
         return 0;
      }

      rd->ready_for_data = 1;
      rd->last_ptr = 0;
      rc = rsnd_start_thread(rd);
      if ( !rc )
      {
         rsound_stop(&rd->io);
         return 0;
      }
   }
	
   return 1;
}

int rsnd_send_chunk(int socket, char* buf, size_t size)
{
	int rc = 0;
   size_t wrote = 0;
   size_t send_size = 0;
   struct pollfd fd;
   fd.fd = socket;
   fd.events = POLLOUT;

   while ( wrote < size )
   {
      if ( poll(&fd, 1, 500) < 0 )
         return 0;

      if ( fd.revents & POLLHUP )
         return 0;

      send_size = (size - wrote) > 1024 ? 1024 : size - wrote;
	   rc = send(socket, buf + wrote, send_size, 0);
      if ( rc <= 0 )
         return 0;

      wrote += rc;
   }
	return wrote;
}

void rsnd_drain(snd_pcm_rsound_t *rd)
{
	if ( rd->has_written )
	{
		int64_t temp, temp2;

		struct timespec now_tv;
		clock_gettime(CLOCK_MONOTONIC, &now_tv);
		
		temp = (int64_t)now_tv.tv_sec - (int64_t)rd->start_tv.tv_sec;
		temp *= rd->rate * rd->bytes_per_frame;

		temp2 = (int64_t)now_tv.tv_nsec - (int64_t)rd->start_tv.tv_nsec;
		temp2 *= rd->rate * rd->bytes_per_frame;
		temp2 /= 1000000000;
		temp += temp2;

      rd->bytes_in_buffer = (int)((int64_t)rd->total_written + (int64_t)rd->buffer_pointer - temp);
   }
	else
      rd->bytes_in_buffer = rd->buffer_pointer;
}

int rsnd_fill_buffer(snd_pcm_rsound_t *rd, const char *buf, size_t size)
{
   if ( !rd->thread_active )
   {
      return -1;
   }

   // Wait until we have a ready buffer
   for (;;)
   {
      pthread_mutex_lock(&rd->thread.mutex);
      if (rd->buffer_pointer + (int)size <= (int)rd->alsa_buffer_size  )
      {
         pthread_mutex_unlock(&rd->thread.mutex);
         break;
      }
      pthread_mutex_unlock(&rd->thread.mutex);

      // get signal from thread to check again
      pthread_mutex_lock(&rd->thread.cond_mutex);
      pthread_cond_wait(&rd->thread.cond, &rd->thread.cond_mutex);
      pthread_mutex_unlock(&rd->thread.cond_mutex);
   }

   pthread_mutex_lock(&rd->thread.mutex);
   memcpy(rd->buffer + rd->buffer_pointer, buf, size);
   rd->buffer_pointer += (int)size;
   pthread_mutex_unlock(&rd->thread.mutex);

   // send signal to thread that buffer has been updated
   pthread_cond_signal(&rd->thread.cond);

   return size;
}

int rsnd_start_thread(snd_pcm_rsound_t *rd)
{
   int rc;
   if ( !rd->thread_active )
   {
      rc = pthread_create(&rd->thread.threadId, NULL, rsnd_thread, rd);
      if ( rc != 0 )
         return 0;
      rd->thread_active = 1;
      return 1;
   }
   else
      return 1;
}

int rsnd_stop_thread(snd_pcm_rsound_t *rd)
{
   int rc;
   if ( rd->thread_active )
   {
      rc = pthread_cancel(rd->thread.threadId);
      pthread_join(rd->thread.threadId, NULL);
      pthread_mutex_unlock(&rd->thread.mutex);
      pthread_mutex_unlock(&rd->thread.cond_mutex);
      if ( rc != 0 )
         return 0;
      rd->thread_active = 0;
      return 1;
   }
   else
      return 1;
}

int rsnd_get_delay(snd_pcm_rsound_t *rd)
{
   pthread_mutex_lock(&rd->thread.mutex);
   rsnd_drain(rd);
   int ptr = rd->bytes_in_buffer;
   pthread_mutex_unlock(&rd->thread.mutex);
   return ptr;
}

int rsnd_get_ptr(snd_pcm_rsound_t *rd)
{

   pthread_mutex_lock(&rd->thread.mutex);
   int ptr = rd->buffer_pointer;
   //rsnd_drain(rd);
   //int server_ptr = rd->bytes_in_buffer;
   pthread_mutex_unlock(&rd->thread.mutex);

   //ptr = (server_ptr > ptr) ? server_ptr : ptr;
   //if ( ptr > (int)rd->alsa_buffer_size )
   //   ptr = rd->alsa_buffer_size;

   return ptr;
}

void* rsnd_thread ( void * thread_data )
{
   snd_pcm_rsound_t *rd = thread_data;
   int rc;
   struct timespec now;
   int nsecs;

// Plays back data as long as there is data in the buffer
   for (;;)
   {
      while ( rd->buffer_pointer >= (int)rd->chunk_size )
      {
         rc = rsnd_send_chunk(rd->conn.socket, rd->buffer, rd->chunk_size);
         if ( rc <= 0 )
         {
            rsound_stop(&rd->io);
            // Buffer has terminated, signal fill_buffer
            pthread_exit(NULL);
         }
         
         if ( !rd->has_written )
         {
            pthread_mutex_lock(&rd->thread.mutex);
            clock_gettime(CLOCK_MONOTONIC, &rd->start_tv);
            rd->has_written = 1;
            pthread_mutex_unlock(&rd->thread.mutex);
         }

         pthread_mutex_lock(&rd->thread.mutex);
         rd->total_written += rc;
         pthread_mutex_unlock(&rd->thread.mutex);

         pthread_mutex_lock(&rd->thread.mutex);
         memmove(rd->buffer, rd->buffer + rd->chunk_size, rd->buffer_size - rd->chunk_size);
         rd->buffer_pointer -= (int)rd->chunk_size;
         pthread_mutex_unlock(&rd->thread.mutex);

         // Buffer has decreased, signal fill_buffer
         pthread_cond_signal(&rd->thread.cond);

                          
      }
      // Wait for the buffer to be filled. Test at least every 5ms.
      clock_gettime(CLOCK_REALTIME, &now);
      nsecs = 5000;      
      now.tv_nsec += nsecs;
      if ( now.tv_nsec >= 1000000000 )
      {
         now.tv_sec++;
         now.tv_nsec -= 1000000000;
      }

      pthread_mutex_lock(&rd->thread.cond_mutex);
      pthread_cond_timedwait(&rd->thread.cond, &rd->thread.cond_mutex, &now);
      pthread_mutex_unlock(&rd->thread.cond_mutex);
   }
}