/*
 * File-related system call implementations.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/limits.h>
#include <kern/seek.h>
#include <kern/stat.h>
#include <lib.h>
#include <uio.h>
#include <proc.h>
#include <current.h>
#include <synch.h>
#include <copyinout.h>
#include <vfs.h>
#include <vnode.h>
#include <openfile.h>
#include <filetable.h>
#include <syscall.h>

/*
 * open() - get the path with copyinstr, then use openfile_open and
 * filetable_place to do the real work.
 */
int
sys_open(const_userptr_t upath, int flags, mode_t mode, int *retval)
{
	struct openfile *file;
	char *kpath = (char*)kmalloc(sizeof(char)*PATH_MAX);
	int result = 0;
	const int allflags = O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_NOCTTY;

	/* 
	* Your implementation of system call open starts here.  
	*
	* Check the design document design/filesyscall.txt for the steps
	*/

	//check for invalid flags
	if(flags == allflags)
	{
		return EINVAL;
	}
	//copy in the supplied pathname
	result = copyinstr(upath, kpath, PATH_MAX, NULL);   
	if(result)
	{
		return EFAULT;
	}
	//open the file
	result = openfile_open(kpath, flags, mode, &file);
	if(result)
	{
		return EFAULT;
	}
	//place the file into curproc's file table
	result  = filetable_place(curproc->p_filetable,file,retval);
	if(result)
	{
		return EMFILE;
	}
	kfree(kpath);

	return result;

	/*
	(void) upath; // suppress compilation warning until code gets written
	(void) flags; // suppress compilation warning until code gets written
	(void) mode; // suppress compilation warning until code gets written
	(void) retval; // suppress compilation warning until code gets written
	(void) allflags; // suppress compilation warning until code gets written
	(void) kpath; // suppress compilation warning until code gets written
	(void) file; // suppress compilation warning until code gets written
	*/
}

/*
 * read() - read data from a file
 */
int
sys_read(int fd, userptr_t buf, size_t size, int *retval)
{
       int result = 0;
       struct openfile *file;
       struct iovec iov;
       struct uio fileUIO;
       
       /* 
        * Your implementation of system call read starts here.  
        *
        * Check the design document design/filesyscall.txt for the steps
        */
       //translate the file descriptor number to an open file object
       result = filetable_get(curproc->p_filetable, fd, &file);
       if(result)
       {
           return result;
       }
       //lock the seek position in the open file (but only for seekable objects)
       lock_acquire(file->of_offsetlock);
       //check for files opened write-only
       if(file->of_accmode == O_WRONLY)
       {
            lock_release(file->of_offsetlock);
            return EACCES;
       }
       //construct a struct: uio
       uio_kinit(&iov, &fileUIO, buf, size, file->of_offset, UIO_READ);
       //call VOP_READ
       result = VOP_READ(file->of_vnode, &fileUIO);
       if(result) 
       {
               return result;
       }
       //update the seek position afterwards
       file->of_offset = fileUIO.uio_offset;
       //unlock and filetable_put()
       lock_release(file->of_offsetlock);
       filetable_put(curproc->p_filetable, fd, file);
       //set the return value correctly
       *retval = size - fileUIO.uio_resid;
    
       /*
       (void) fd; // suppress compilation warning until code gets written
       (void) buf; // suppress compilation warning until code gets written
       (void) size; // suppress compilation warning until code gets written
       (void) retval; // suppress compilation warning until code gets written
       */

       return result;
}

/*
 * write() - write data to a file
 */
int
sys_write(int fd, userptr_t buf, size_t size, int *retval)
{
        struct openfile *file;
        struct iovec iov;
        struct uio fileUIO;
	int result = 0;
       
	//translate the file descriptor number to an open file object
	result = filetable_get(curproc->p_filetable, fd, &file);
	if(result)
	{
	   return result;
	}
	//lock the seek position in the open file (but only for seekable objects)
	lock_acquire(file->of_offsetlock);
	//check for files opened read-only
	if(file->of_accmode == O_RDONLY)
	{
	    lock_release(file->of_offsetlock);
	    return EACCES;
	}
	//construct a struct: uio
	uio_kinit(&iov, &fileUIO, buf, size, file->of_offset, UIO_WRITE);
	//call VOP_WRITE
	result = VOP_WRITE(file->of_vnode, &fileUIO);
	if(result) 
	{
	       return result;
	}
	//update the seek position afterwards
	file->of_offset = fileUIO.uio_offset;
	//unlock and filetable_put()
	lock_release(file->of_offsetlock);
        filetable_put(curproc->p_filetable, fd, file);
	//set the return value correctly
	*retval = size - fileUIO.uio_resid;
    
       return result;
}

/*
 * close() - remove from the file table.
 */
int
sys_close(int fd) 
{
	struct openfile* file;
	int result = 0;

	//validate the fd number (use filetable_okfd)
	KASSERT(filetable_okfd(curproc->p_filetable, fd));
	//use filetable_placeat to replace curproc's file table entry with NULL
	filetable_placeat(curproc->p_filetable, NULL, fd, &file);
	//check if the previous entry in the file table was also NULL
	if(file == NULL)
	{
		return ENOENT;
	}
	else
	{
	//decref the open file returned by filetable_placeat
		openfile_decref(file);
	}

	return result;
}



/* 
* meld () - combine the content of two files word by word into a new file
*/
int sys_meld(const_userptr_t pn1, const_userptr_t pn2, const_userptr_t pn3, int *retval)
{
	struct openfile *file1, *file2, *file3;
	struct iovec iov;
	struct uio file1UIO, file2UIO, writeUIO;
	struct stat stat_struct;
	char *kpath_pn1 = (char*)kmalloc(sizeof(char)*PATH_MAX);
	char *kpath_pn2 = (char*)kmalloc(sizeof(char)*PATH_MAX);
	char *kpath_pn3 = (char*)kmalloc(sizeof(char)*PATH_MAX);
	char *buffer1 = (char*)kmalloc(sizeof(char)*4);
	char *buffer2 = (char*)kmalloc(sizeof(char)*4);
	int result = 0;
	int counter = 0;
	int fd, fd_ret, size;

	//copy in the supplied pathnames (pn1, pn2, and pn3)
	result = copyinstr(pn1, kpath_pn1, PATH_MAX, NULL);
	if(result)
	{
		return result;
	}
	result = copyinstr(pn2, kpath_pn2, PATH_MAX, NULL);
	if(result)
	{	
		return result;
	}
	result = copyinstr(pn3, kpath_pn3, PATH_MAX, NULL);
	if(result)
	{
		return result;
	}
	//open the first two files (use openfile_open) for reading
	//return if any file is not open'ed correctly
	result = openfile_open(kpath_pn1, O_RDWR, 0664, &file1);
	if(result)
	{
		return ENOENT;
	}
	result = openfile_open(kpath_pn2, O_RDWR, 0664, &file2);
	if(result)
	{
		return ENOENT;
	}
	//open the third file (use openfile_open) for writing
	//return if any file is not open'ed correctly
	result = openfile_open(kpath_pn3, O_WRONLY | O_CREAT | O_EXCL, 0664, &file3);
	if(result)
	{
		return EEXIST;
	}
	//place them into curproc's file table (use filetable_place)
	result = filetable_place(curproc->p_filetable, file1, &fd_ret);
	if(result)
	{
		return result;
	}
	result = filetable_place(curproc->p_filetable, file2, &fd_ret);
	if(result)
	{
		return result;
	}
	result = filetable_place(curproc->p_filetable, file3, &fd_ret);
	if(result)
	{
		return result;
	}
	fd = fd_ret;
	
	//size of file1
	result = VOP_STAT(file1->of_vnode, &stat_struct);
	size = stat_struct.st_size;
	//size of file2
	result = VOP_STAT(file2->of_vnode, &stat_struct);
	size = size + stat_struct.st_size;

	while(counter < (size/2))
	{
		//file1 reading 4 bytes
		lock_acquire(file1->of_offsetlock);
		
		uio_kinit(&iov, &file1UIO, buffer1, 4, file1->of_offset, UIO_READ);
		result = VOP_READ(file1->of_vnode, &file1UIO);
		if(result)
		{
			return result;
		}
		file1->of_offset = file1UIO.uio_offset;

		lock_release(file1->of_offsetlock);
		
		//file2 reading 4 bytes
		lock_acquire(file2->of_offsetlock);
		uio_kinit(&iov, &file2UIO, buffer2, 4, file2->of_offset, UIO_READ);
		result = VOP_READ(file2->of_vnode, &file2UIO);
		if(result)
		{
			return result;
		}
		file2->of_offset = file2UIO.uio_offset;
		
		lock_release(file2->of_offsetlock);

		//write 4 bytes from file 1 to the meld file
		lock_acquire(file3->of_offsetlock);
		uio_kinit(&iov, &writeUIO, buffer1, 4,file3->of_offset, UIO_WRITE);
		result = VOP_WRITE(file3->of_vnode, &writeUIO);
		if(result)
		{
			return result;
		}
		file3->of_offset = writeUIO.uio_offset;
		
		lock_release(file3->of_offsetlock);

		//write 4 bytes from file 2 to the meld file
		lock_acquire(file3->of_offsetlock);
		uio_kinit(&iov, &writeUIO, buffer2, 4, file3->of_offset, UIO_WRITE);
		result = VOP_WRITE(file3->of_vnode, &writeUIO);
		if(result)
		{
			return result;
		}

		file3->of_offset = writeUIO.uio_offset;
		lock_release(file3->of_offsetlock);
	
		counter = counter + 4;
	}

	//set the return value correctly for successful completion
	*retval = file3->of_offset;
	//close files and clean up
	KASSERT(filetable_okfd(curproc->p_filetable, fd));

	filetable_placeat(curproc->p_filetable, NULL, fd, &file1);
	filetable_placeat(curproc->p_filetable, NULL, fd, &file2);
	filetable_placeat(curproc->p_filetable, NULL, fd, &file3);
	
	if(file1 == NULL)
	{
		return ENOENT;
	}
	if(file2 == NULL)
	{
		return ENOENT;
	}
	if(file3 == NULL)
	{
		return ENOENT;
	}
	
	openfile_decref(file1);
	openfile_decref(file2);
	openfile_decref(file3);

	kfree(kpath_pn1);
	kfree(kpath_pn2);
	kfree(kpath_pn3);
	kfree(buffer1);
	kfree(buffer2);

	return 0;
}