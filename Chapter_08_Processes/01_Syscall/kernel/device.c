/*! Devices - common interface implementation */
#define _K_DEVICE_C_

#include "device.h"

#include <kernel/errno.h> /* shares errno with arch layer */
#include "memory.h"
#include <arch/interrupt.h>
#include <lib/string.h>

static list_t devices;

/*! Initialize initial device as console for system boot messages */
void kdevice_set_initial_stdout ()
{
	static kdevice_t k_initial_stdout;
	extern device_t K_INITIAL_STDOUT;
	extern void *k_stdout; /* console for kernel messages */

	k_initial_stdout.dev = K_INITIAL_STDOUT;
	k_initial_stdout.dev.init ( 0, NULL, &k_initial_stdout.dev );
	k_stdout = &k_initial_stdout;
}

/*! Initialize 'device' subsystem */
int k_devices_init ()
{
	extern device_t DEVICES_DEV; /* defined in arch/devices, Makefile */
	device_t *dev[] = { DEVICES_DEV_PTRS, NULL };
	kdevice_t *kdev;
	int iter;

	list_init ( &devices );

	for ( iter = 0; dev[iter] != NULL; iter++ )
	{
		kdev = k_device_add ( dev[iter] );
		k_device_init ( kdev, 0, NULL, NULL );
	}

	return 0;
}

/*! Add new device to system */
kdevice_t *k_device_add ( device_t *dev )
{
	kdevice_t *kdev;

	ASSERT ( dev );

	kdev = kmalloc ( sizeof (kdevice_t) );
	ASSERT ( kdev );

	kdev->dev = *dev;
	kdev->id = k_new_id ();
	kdev->flags = 0;

	list_append ( &devices, kdev, &kdev->list );

	return kdev;
}

/*! Initialize device (and call its initializer, if set) */
int k_device_init ( kdevice_t *kdev, int flags, void *params, void *callback )
{
	int retval = 0;

	ASSERT ( kdev );

	if ( flags )
		kdev->dev.flags = flags;

	if ( params )
		kdev->dev.params = params;

	kdev->locked = FALSE;
	kthreadq_init ( &kdev->thrq );
	list_init ( &kdev->descriptors );

	if ( kdev->dev.init )
		retval = kdev->dev.init ( flags, params, &kdev->dev );

	if ( !retval && kdev->dev.irq_handler )
	{
		(void) arch_register_interrupt_handler ( kdev->dev.irq_num,
							 kdev->dev.irq_handler,
							 &kdev->dev );
		arch_irq_enable ( kdev->dev.irq_num );
	}

	if ( callback )
		kdev->dev.callback = callback;

	return retval;
}

/*! Remove device from list of devices */
int k_device_remove ( kdevice_t *kdev )
{
#ifdef DEBUG
	kdevice_t *test;
#endif
	ASSERT ( kdev );

	if ( kdev->dev.irq_num != -1 )
		arch_irq_disable ( kdev->dev.irq_num );

	if ( kdev->dev.irq_handler )
		arch_unregister_interrupt_handler ( kdev->dev.irq_num,
						    kdev->dev.irq_handler,
						    &kdev->dev );
	if ( kdev->dev.destroy )
		kdev->dev.destroy ( kdev->dev.flags, kdev->dev.params,
				    &kdev->dev );
#ifdef DEBUG
	test = list_find_and_remove ( &devices, &kdev->list );
	ASSERT ( test == kdev );
#else
	(void) list_remove ( &devices, 0, &kdev->list );
#endif

	k_free_id ( kdev->id );

	kfree ( kdev );

	return 0;
}

/*! Send data to device */
int k_device_send ( void *data, size_t size, int flags, kdevice_t *kdev )
{
	int retval;

	if ( kdev->dev.send )
		retval = kdev->dev.send ( data, size, flags, &kdev->dev );
	else
		retval = EXIT_FAILURE;

	return retval;
}

/*! Read data from device */
int k_device_recv ( void *data, size_t size, int flags, kdevice_t *kdev )
{
	int retval;

	if ( kdev->dev.recv )
		retval = kdev->dev.recv ( data, size, flags, &kdev->dev );
	else
		retval = EXIT_FAILURE;

	return retval;
}

/*! Open device with 'name' (for exclusive use, if defined) */
kdevice_t *k_device_open ( char *name, int flags )
{
	kdevice_t *kdev;

	kdev = list_get ( &devices, FIRST );
	while ( kdev )
	{
		if ( !strcmp ( name, kdev->dev.dev_name ) )
		{
			if ( 	(kdev->dev.flags & DEV_TYPE_NOTSHARED) &&
				(kdev->dev.flags & DEV_OPEN) )
				return NULL;

			/* FIXME: check read/write/exclusive open conflicts */

			kdev->flags |= DEV_OPEN | flags;
			kdev->ref_cnt++;

			return kdev;
		}

		kdev = list_get_next ( &kdev->list );
	}

	return NULL;
}

/*! Close device (close exclusive use, if defined) */
void k_device_close ( kdevice_t *kdev )
{
	kdev->ref_cnt--;
	if ( !kdev->ref_cnt )
		kdev->flags &= ~DEV_OPEN;

	/* FIXME: restore flags; use list kdev->descriptors? */
}

/*! Lock device */
int k_device_lock ( kdevice_t *dev, int wait )
{
	if ( !wait && dev->locked )
		return -1;

	if ( dev->locked )
	{
		kthread_enqueue ( NULL, &dev->thrq );
		kthreads_schedule ();
	}

	dev->locked = TRUE;

	return 0;
}

/*! Unlock device */
int k_device_unlock ( kdevice_t *dev )
{
	if ( kthreadq_release ( &dev->thrq ) )
		kthreads_schedule ();
	else
		dev->locked = FALSE;

	return 0;
}

/* /dev/null emulation */
static int do_nothing ()
{
	return 0;
}

device_t dev_null = (device_t)
{
	.dev_name = "dev_null",

	.irq_num = 	-1,
	.irq_handler =	NULL,

	.init =		NULL,
	.destroy =	NULL,
	.send =		do_nothing,
	.recv =		do_nothing,

	.flags = 	DEV_TYPE_SHARED,
	.params = 	NULL,
};

/*! syscall wrappers -------------------------------------------------------- */

int sys__open ( void *p )
{
	char *pathname;
	int flags;
	/* mode_t mode - not used */
	descriptor_t *desc;

	kdevice_t *kdev;
	kobject_t *kobj;

	pathname =	*( (char **) p );		p += sizeof (char *);
	flags =		*( (int *) p );			p += sizeof (int);
	/* pathname =	*( (char **) p ); */		p += sizeof (mode_t);
	desc =		*( (descriptor_t **) p );

	ASSERT_ERRNO_AND_EXIT ( pathname, EINVAL );
	ASSERT_ERRNO_AND_EXIT ( desc, EINVAL );

	kdev = k_device_open ( pathname, flags );

	if ( !kdev )
		return EXIT_FAILURE;

	kobj = kmalloc_kobject ( 0 );
	ASSERT_ERRNO_AND_EXIT ( kobj, ENOMEM );

	kobj->kobject = kdev;
	kobj->flags = flags;

	desc->ptr = kobj;
	desc->id = kdev->id;

	/* add descriptor to device list */
	list_append ( &kdev->descriptors, kobj, &kobj->spec );

	EXIT2 ( EXIT_SUCCESS, EXIT_SUCCESS );
}

int sys__close ( void *p )
{
	descriptor_t *desc;

	kdevice_t *kdev;
	kobject_t *kobj;

	desc = *( (descriptor_t **) p );

	ASSERT_ERRNO_AND_EXIT ( desc, EINVAL );

	kobj = desc->ptr;
	ASSERT_ERRNO_AND_EXIT ( kobj, EINVAL );
	ASSERT_ERRNO_AND_EXIT ( list_find ( &kobjects, &kobj->list ),
				EINVAL );
	kdev = kobj->kobject;
	ASSERT_ERRNO_AND_EXIT ( kdev && kdev->id == desc->id, EINVAL );

	kobj->kobject = NULL;
	kfree_kobject ( kobj );

	/* remove descriptor from device list */
	list_remove ( &kdev->descriptors, 0, &kobj->spec );

	k_device_close ( kdev );

	EXIT2 ( EXIT_SUCCESS, EXIT_SUCCESS );
}

static int read_write ( void *p, int op );

int sys__read ( void *p )
{
	return read_write ( p, TRUE );
}
int sys__write ( void *p )
{
	return read_write ( p, FALSE );
}

static int read_write ( void *p, int op )
{
	descriptor_t *desc;
	void *buf;
	size_t count;

	kdevice_t *kdev;
	kobject_t *kobj;
	int retval;

	desc =  *( (descriptor_t **) p );	p += sizeof (descriptor_t *);
	buf =   *( (char **) p );		p += sizeof (char *);
	count = *( (size_t *) p );

	ASSERT_ERRNO_AND_EXIT ( desc && buf && count > 0, EINVAL );

	kobj = desc->ptr;
	ASSERT_ERRNO_AND_EXIT ( kobj, EINVAL );
	ASSERT_ERRNO_AND_EXIT ( list_find ( &kobjects, &kobj->list ),
				EINVAL );
	kdev = kobj->kobject;
	ASSERT_ERRNO_AND_EXIT ( kdev && kdev->id == desc->id, EINVAL );

	/* TODO check permission for requested operation from opening flags */

	if ( op )
		retval = k_device_recv ( buf, count, kobj->flags, kdev );
	else
		retval = k_device_send ( buf, count, kobj->flags, kdev );

	if ( retval >= 0 )
		EXIT2 ( EXIT_SUCCESS, EXIT_SUCCESS );
	else
		EXIT2 ( EIO, EXIT_FAILURE );
}