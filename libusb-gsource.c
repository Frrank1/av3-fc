/*
 * libusb-gsource.c
 *
 * Functions for integrating libusb with a glib main event loop. Also some
 * useful libusb utils.
 */

#include <libusb-1.0/libusb.h>
#include <glib-2.0/glib.h>
#include <time.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>

#include "libusb-gsource.h"

//todo: document
//todo: are there error types I can handle here?
//todo: multithread libusb_handle_events_timeout_completed

struct libusbSource {
	GSource source;
	GSList * devices;
	GSList * fds;
	int timeout_error;
	int handle_events_error;
	libusb_context * context;
};

static gboolean prepare(GSource *g_source, gint *timeout_)
{
	libusbSource *usb_src = (libusbSource *)g_source;
	struct timeval timeout;
    int retval = libusb_get_next_timeout(usb_src->context, &timeout);
    switch(retval){
		case 0:
			*timeout_ = -1;
			return FALSE;
		case 1:
			*timeout_  =  timeout.tv_sec*1000 + timeout.tv_usec/1000; //wait at most timeout, rounded down to nearest msec
			return *timeout_ == 0 ? TRUE : FALSE;
		default:
			usb_src->timeout_error = retval;
			return TRUE;
	}
    return FALSE;
}

static gboolean alt_prepare(GSource *g_source, gint *timeout_){
	*timeout_ = -1;
	return FALSE;
}

static gboolean check(GSource *g_source)
{
	libusbSource *usb_src = (libusbSource *)g_source;
	GSList * elem = usb_src->fds;
	GPollFD * g_usb_fd = NULL;

	if(!elem)
		return FALSE;

	do{
		g_usb_fd = elem->data;
		if(g_usb_fd->revents)
			return TRUE;
	}while((elem = g_slist_next(elem)));

	return FALSE;
}

static gboolean dispatch(GSource *g_source, GSourceFunc callback, gpointer user_data)
{
	// from some random bit of code on the interwebs:
	// If Dispatch returns FALSE, GLib will destroy the source.
	// src.chromium.org/svn/trunk/src/base/message_pump_glib.cc

	libusbSource *usb_src = (libusbSource *)g_source;
	libusbSourceErrorCallback errCB = (libusbSourceErrorCallback)callback;
	struct timeval nonblocking = {
			.tv_sec = 0,
			.tv_usec = 0,
	};

	if(usb_src->timeout_error && errCB != NULL){
		errCB(usb_src->timeout_error, 0, user_data);
		return TRUE;
	}

	usb_src->handle_events_error = libusb_handle_events_timeout(usb_src->context, &nonblocking);
	if(usb_src->handle_events_error && errCB != NULL)
		errCB(0, usb_src->handle_events_error, user_data);
    return TRUE;
}

static void close_device(gpointer data, gpointer user_data){
	libusb_device_handle * handle = data;
	libusb_close(handle);
}

static void free_pollfd(gpointer data){
	g_slice_free(GPollFD, data);
}

static void finalize(GSource *g_source){
	libusbSource * usb_src = (libusbSource*)g_source;

	/* Since this GSource is finalizing, the pollfds are already
	 * removed. Simply free them all so we don't try to remove
	 * them from the source again. */
	g_slist_free_full(usb_src->fds, free_pollfd);
	usb_src->fds = NULL;

	g_slist_foreach(usb_src->devices, close_device, NULL);
	libusb_exit(usb_src->context);
}

static void usb_fd_added_cb(int fd, short events, void * source){
	libusbSource * usb_src = (libusbSource *)source;
	GPollFD * g_usb_fd = g_slice_new0(GPollFD);

	g_usb_fd->fd = fd;
	if (events & POLLIN)
			g_usb_fd->events |= G_IO_IN;
	if (events & POLLOUT)
			g_usb_fd->events |= G_IO_OUT;

	usb_src->fds = g_slist_prepend(usb_src->fds, g_usb_fd);
	g_source_add_poll((GSource *)usb_src, g_usb_fd);
}

static void usb_fd_removed_cb(int fd, void* source){
	libusbSource * usb_src = (libusbSource *)source;
	GSList * elem = usb_src->fds;
	GPollFD * g_usb_fd = NULL;

	if(!elem)
		return; //error? asked to remove an fd that wasn't being used in a poll

	do{
		g_usb_fd = elem->data;
		if(g_usb_fd->fd == fd){
			g_source_remove_poll((GSource*)source, g_usb_fd);
			free_pollfd(g_usb_fd);
			usb_src->fds = g_slist_delete_link(usb_src->fds, elem);
			break;
		}
	}while((elem = g_slist_next(elem)));
}

static int init_usb_fds(libusbSource * usb_source){
	int numfds = 0;
	const struct libusb_pollfd ** usb_fds = libusb_get_pollfds(usb_source->context);
	if(!usb_fds)
		return -1;

	for(numfds = 0; usb_fds[numfds] != NULL; ++numfds){
		usb_fd_added_cb(usb_fds[numfds]->fd, usb_fds[numfds]->events, usb_source);
	}

	free(usb_fds);
	libusb_set_pollfd_notifiers(usb_source->context, usb_fd_added_cb, usb_fd_removed_cb, usb_source);
	return 0;
}

libusbSource * libusbSource_new(void){
	libusb_context * context;
	int usbErr = libusb_init(&context);
	if(usbErr){
		print_libusb_error(usbErr, "libusb_init");
		return NULL;
	}
	libusb_set_debug(context, 3);

	static GSourceFuncs usb_funcs = {prepare, check, dispatch, finalize};
	if(!libusb_pollfds_handle_timeouts(context)){
		usb_funcs.prepare = alt_prepare;
	}

	GSource * g_usb_source = g_source_new (&usb_funcs, sizeof(libusbSource));
	libusbSource * usb_source = (libusbSource *) g_usb_source;

	usb_source->devices = NULL; //important to set null because g_source_destroy calls finalize
	usb_source->fds = NULL; //important to set null because g_source_destroy calls finalize
	usb_source->context = context;
	usb_source->timeout_error = 0;
	usb_source->handle_events_error = 0;

	if(init_usb_fds(usb_source)){
		g_source_destroy((GSource*)usb_source);
		return NULL;
	}

	return usb_source;
}

static libusb_device * find_usb_device(libusbSource * usb_source, is_device is_device){
	libusb_device **list = NULL;
	libusb_device *found = NULL;
	ssize_t num_usb_dev = 0;
	ssize_t i = 0;

	num_usb_dev = libusb_get_device_list(usb_source->context, &list);
	if(num_usb_dev < 0){
		print_libusb_error(num_usb_dev, "Could not get device list");
		return NULL;
	}
	//look through the list for the device matching is_device
	for(i = 0; i < num_usb_dev; ++i){
		if(is_device(list[i]) == TRUE){
			found = list[i];
			libusb_ref_device(found);
			break;
		}
	}
	if(!found){
		fprintf(stderr, "Device not found\n");
	}

	libusb_free_device_list(list, 1);
	return found;
}


static libusb_device_handle * open_device_interface(libusb_device * dev, int * iface_num, int num_ifaces){
	libusb_device_handle *handle = NULL;
	int i = 0;
	int retErr = 0;
	int kd_stat = 0;
	if(!dev)
		return NULL;
	retErr = libusb_open(dev, &handle);
	if(retErr){
		print_libusb_error(retErr, "Could not open device");
		return NULL;
	}
	//claim requested interfaces on the device
	for(i=0; i < num_ifaces; ++i){
		//if the kernel driver is active on the interfaces we want, detach it
		kd_stat = libusb_kernel_driver_active(handle, iface_num[i]);
		if(kd_stat < 0){
			print_libusb_error(kd_stat,"Failure finding kernel driver status");
			libusb_close(handle);
			return NULL;
		}
		if(kd_stat > 0){ //the kernel driver is active (kd_stat = 1)
			retErr = libusb_detach_kernel_driver(handle, iface_num[i]);
			if(retErr){
				print_libusb_error(retErr, "Could not detach kernel driver");
				libusb_close(handle);
				return NULL;
			}
		}

		retErr = libusb_claim_interface(handle, iface_num[i]);
		if(retErr){
			print_libusb_error(retErr, "Could not claim device interface");
			libusb_attach_kernel_driver(handle, iface_num[i]);
			libusb_close(handle);
			return NULL;
		}
	}

	return handle;
}

libusb_device_handle * open_usb_device_handle(libusbSource * usb_source,
    is_device is_device, int * iface_num, int num_ifaces)
{
	libusb_device * dev = find_usb_device(usb_source, is_device);
	return open_device_interface(dev, iface_num,num_ifaces);

}

void print_libusb_error(int libusberrno, const char* str) {
	switch(libusberrno) {
    case LIBUSB_SUCCESS:
		fprintf(stderr, "**%s: SUCCESS\n",str);
		break;
    case LIBUSB_ERROR_IO:
		fprintf(stderr, "**%s: ERROR_IO\n",str);
		break;
	case LIBUSB_ERROR_INVALID_PARAM:
		fprintf(stderr, "**%s: ERROR_INVALID_PARAM\n",str);
		break;
	case LIBUSB_ERROR_ACCESS:
		fprintf(stderr, "**%s: ERROR_ACCESS\n",str);
		break;
	case LIBUSB_ERROR_NO_DEVICE:
		fprintf(stderr, "**%s: ERROR_NO_DEVICE\n",str);
		break;
	case LIBUSB_ERROR_NOT_FOUND:
		fprintf(stderr, "**%s: ERROR_NOT_FOUND\n",str);
		break;
	case LIBUSB_ERROR_BUSY:
	   fprintf(stderr, "**%s: ERROR_BUSY\n",str);
	   break;
    case LIBUSB_ERROR_TIMEOUT:
		fprintf(stderr, "**%s: ERROR_TIMEOUT\n",str);
		break;
	case LIBUSB_ERROR_OVERFLOW:
		fprintf(stderr, "**%s: ERROR_OVERFLOW\n",str);
		break;
	case LIBUSB_ERROR_PIPE:
		fprintf(stderr, "**%s: ERROR_PIPE\n",str);
		break;
	case LIBUSB_ERROR_INTERRUPTED:
		fprintf(stderr, "**%s: ERROR_INTERRUPTED\n",str);
		break;
	case LIBUSB_ERROR_NO_MEM:
		fprintf(stderr, "**%s: ERROR_NO_MEM\n",str);
		break;
	case LIBUSB_ERROR_NOT_SUPPORTED:
		fprintf(stderr, "**%s: ERROR_NOT_SUPPORTED\n",str);
		break;
	case LIBUSB_ERROR_OTHER:
		fprintf(stderr, "**%s: ERROR_OTHER\n",str);
		break;
	default:
		fprintf(stderr, "***%s:  unknown error %i ***\n", str, libusberrno);
		break;
    }
/*  fprintf(stderr, "**%s: %s, %d\n", str, libusb_error_name(libusberrno),
 *	        libusberrno);
 *  libusb_error_name() only occurs in libusb1.0.9, 1.0.8 is common
 */
}

void print_libusb_transfer_error(int status, const char* str){
	switch(status){
	case LIBUSB_TRANSFER_COMPLETED:
		fprintf(stderr, "**%s: LIBUSB_TRANSFER_COMPLETED\n", str);
		break;
	case LIBUSB_TRANSFER_ERROR:
		fprintf(stderr, "**%s: LIBUSB_TRANSFER_ERROR\n", str);
		break;
	case LIBUSB_TRANSFER_TIMED_OUT:
		fprintf(stderr, "**%s: LIBUSB_TRANSFER_TIMED_OUT\n", str);
		break;
	case LIBUSB_TRANSFER_CANCELLED:
		fprintf(stderr, "**%s: LIBUSB_TRANSFER_CANCELLED\n", str);
		break;
	case LIBUSB_TRANSFER_STALL:
		fprintf(stderr, "**%s: LIBUSB_TRANSFER_STALL\n", str);
		break;
	case LIBUSB_TRANSFER_NO_DEVICE:
		fprintf(stderr, "**%s: LIBUSB_TRANSFER_NO_DEVICE\n", str);
		break;
	case LIBUSB_TRANSFER_OVERFLOW:
		fprintf(stderr, "**%s: LIBUSB_TRANSFER_OVERFLOW\n", str);
		break;
	default:
		fprintf(stderr, "***%s: Unknown transfer status %i***\n", str, status);
		break;
	}
}
