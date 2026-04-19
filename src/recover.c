/***************************************************************************
 *   Copyright (C) 2010 by Roberto Maar                                    *
 *   robi6@users.sf.net                                                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>.  *
 ***************************************************************************/
//header  util.h

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h> 
#include <errno.h>
#include <fcntl.h>
#include <utime.h>
#include <sys/sysmacros.h>

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

#include <ext2fs/ext2fs.h>
#include <ext2fs/ext2_ext_attr.h>
#include "util.h"
#include "hard_link_stack.h"
#include "inode.h"
#include "block.h"


//defines for error in recover_file
#define CREATE_ERROR	0x80
#define SEEK_ERROR	0x40
#define MOVE_ERROR	0x20
#define CHOWN_ERROR	0x10
#define CHMOD_ERROR	0x08
#define UTIME_ERROR	0x04
#define ATTRI_ERROR	0x02

//in this version unused
#define ACL_ERROR	0x01


#ifdef FILE_ATTR
#include <e2p/e2p.h>
//#FLAGS_MODIFIABLE = EXT2_NOATIME_FL | EXT2_SYNC_FL | EXT2_DIRSYNC_FL | EXT2_APPEND_FL | EXT2_COMPR_FL | EXT2_NODUMP_FL |
// EXT2_IMMUTABLE_FL | EXT3_JOURNAL_DATA_FL | EXT2_SECRM_FL | EXT2_UNRM_FL | EXT2_NOTAIL_FL | EXT2_TOPDIR_FL 
#define FLAGS_MODIFIABLE	0x0001E0FF
#endif


extern ext2_filsys current_fs;

static int block_bitmap_test(ext2fs_block_bitmap bitmap, blk64_t block)
{
	return ext2fs_test_block_bitmap2(bitmap, block);
}


static void block_bitmap_mark(ext2fs_block_bitmap bitmap, blk64_t block)
{
	ext2fs_mark_block_bitmap2(bitmap, block);
}


struct privat {
	int fd;
	char* buf;
	char flag; 
	int error;};


struct alloc_stat{
	__u32 allocated;
	__u32 not_allocated;};


struct alloc_recover_stat{
	__u32 allocated;
	__u32 recovered;};

#define EXT4_XATTR_INDEX_SYSTEM 7


static int inode_has_inline_data(struct ext2_inode *inode)
{
	return (inode->i_flags & EXT4_INLINE_DATA_FL) != 0;
}


static int read_inline_data(ext2_ino_t inode_nr, struct ext2_inode *inode,
			    char **buf_out, size_t *size_out)
{
	struct ext2_inode_large *large_inode;
	struct ext2_ext_attr_entry *entry;
	__u32 *magic;
	char *buf;
	char *start;
	char *end;
	char *value;
	size_t size;
	size_t copied;
	unsigned int inode_size;
	unsigned int extra_isize;
	unsigned int value_offs;
	unsigned int value_size;
	unsigned int storage_size;
	__u64 i_size;

	*buf_out = NULL;
	*size_out = 0;

	if (!inode_has_inline_data(inode))
		return 1;

	i_size = inode->i_size | ((unsigned long long) inode->i_size_high << 32);
	size = (size_t) i_size;
	if (!size)
		return 0;

	buf = malloc(size + 1);
	if (!buf)
		return ENOMEM;
	memset(buf, 0, size + 1);

	copied = (size > EXT4_MIN_INLINE_DATA_SIZE) ? EXT4_MIN_INLINE_DATA_SIZE : size;
	memcpy(buf, inode->i_block, copied);
	if (copied == size) {
		*buf_out = buf;
		*size_out = size;
		return 0;
	}

	inode_size = EXT2_INODE_SIZE(current_fs->super);
	if (inode_size <= EXT2_GOOD_OLD_INODE_SIZE) {
		free(buf);
		return EXT2_ET_NO_INLINE_DATA;
	}

	large_inode = (struct ext2_inode_large *) inode;
	extra_isize = ext2fs_le16_to_cpu(large_inode->i_extra_isize);
	if (extra_isize > inode_size - EXT2_GOOD_OLD_INODE_SIZE) {
		free(buf);
		return EXT2_ET_NO_INLINE_DATA;
	}

	storage_size = inode_size - EXT2_GOOD_OLD_INODE_SIZE - extra_isize;
	magic = (__u32 *)((char *) inode + EXT2_GOOD_OLD_INODE_SIZE + extra_isize);
	if (storage_size < sizeof(__u32) ||
	    ext2fs_le32_to_cpu(*magic) != EXT2_EXT_ATTR_MAGIC) {
		free(buf);
		return EXT2_ET_NO_INLINE_DATA;
	}

	start = (char *) magic + sizeof(__u32);
	end = (char *) inode + inode_size;
	entry = (struct ext2_ext_attr_entry *) start;
	while (!EXT2_EXT_IS_LAST_ENTRY(entry)) {
		if ((char *) entry + sizeof(struct ext2_ext_attr_entry) > end) {
			free(buf);
			return EXT2_ET_NO_INLINE_DATA;
		}
		if (entry->e_name_index == EXT4_XATTR_INDEX_SYSTEM &&
		    entry->e_name_len == 4 &&
		    !memcmp(EXT2_EXT_ATTR_NAME(entry), "data", 4)) {
			value_offs = ext2fs_le16_to_cpu(entry->e_value_offs);
			value_size = ext2fs_le32_to_cpu(entry->e_value_size);
			value = start + value_offs;
			if ((value_offs > storage_size) ||
			    (value_size > storage_size) ||
			    (value + value_size > end)) {
				free(buf);
				return EXT2_ET_NO_INLINE_DATA;
			}
			if (copied + value_size < copied || copied + value_size < value_size) {
				free(buf);
				return EXT2_ET_NO_INLINE_DATA;
			}
			if (copied + value_size < size)
				size = copied + value_size;
			memcpy(buf + copied, value, size - copied);
			*buf_out = buf;
			*size_out = size;
			return 0;
		}
		entry = EXT2_EXT_ATTR_NEXT(entry);
	}

	free(buf);
	return EXT2_ET_NO_INLINE_DATA;
}



// recover files from a "double quotes" listfile
void recover_list(char *des_dir, char *input_file,__u32 t_after, __u32 t_before, int flag){
	FILE  *f;
	char *lineptr = NULL ;
	char *filename = NULL ;
	char *p1 , *p2;
	size_t maxlen;
	size_t got;
	ext2_ino_t inode_nr;
	struct ring_buf *i_list;
	struct ext2_inode* r_inode;
	r_item *item = NULL;	
	
	f = fopen(input_file,"r");
	if (f) {
		rewind(f);
		maxlen = 512;
		lineptr = malloc(maxlen);
		filename = malloc(maxlen);
		if ((!lineptr) || (!filename))
			goto errout;
		while (! (feof(f))){
			got = getline (&lineptr, &maxlen, f);
		 	if (got != -1){
				p1 = strchr(lineptr,'"');
				p2 = strrchr(lineptr,'"');
				if ((p1) && (p2) && (p1 != p2)){
					p1++;
					*p2 = 0;
					strcpy(filename,p1);
					if (*filename == 0)
						continue;
					inode_nr = local_namei(NULL,filename,t_after,t_before,DELETED_OPT);
					if(! inode_nr){
#ifdef DEBUG
						printf("Filename %s not found\n",filename);
#endif
						continue;
					}
					i_list = get_j_inode_list(current_fs->super, inode_nr);
					item = get_undel_inode(i_list,t_after,t_before);

					if (item) {
						r_inode = (struct ext2_inode*)item->inode;
						if (! LINUX_S_ISDIR(r_inode->i_mode) ) 
							recover_file(des_dir,"", filename, r_inode, inode_nr ,flag);
						}
#ifdef DEBUG
						else
							printf("no Inode found for %s\n",filename);
#endif
					if (i_list) ring_del(i_list);
				}
#ifdef DEBUG
				else
					printf("Filename not found in : \"%s\"",lineptr);
#endif	
			}
		}

	}

errout:
	if (f) fclose(f);
	if (lineptr)
		free(lineptr);
	if (filename)
		free(filename); 
return ;
}


// Subfunction for "local_block_iterate3()" for check if blocks not allocated and not recovered
 static int check_block_stat(ext2_filsys fs, blk64_t *blocknr, e2_blkcnt_t blockcnt,
                  blk64_t /*ref_blk*/x, int /*ref_offset*/y, void *priv )
{
//FIXME: 
	if (*blocknr >= fs->super->s_blocks_count)
		return BLOCK_ERROR;
	struct alloc_recover_stat *stat = priv;
        if (block_bitmap_test(fs->block_map, *blocknr))
		(stat->allocated)++ ;	
	if (bmap && block_bitmap_test(bmap, *blocknr))
		(stat->recovered)++ ;
return 0;
}


// Subfunction for "local_block_iterate3()" for check if the blocks allocated
 static int check_block(ext2_filsys fs, blk64_t *blocknr, e2_blkcnt_t blockcnt,
                  blk64_t /*ref_blk*/x, int /*ref_offset*/y, void *priv )
{
//FIXME: 
	if (*blocknr >= fs->super->s_blocks_count)
		return BLOCK_ERROR;
	struct alloc_stat *stat = priv;
        if (block_bitmap_test(fs->block_map, *blocknr))
		(stat->allocated)++ ;	
	else
		(stat->not_allocated)++ ;
	if (bmap)
		block_bitmap_mark(bmap, *blocknr);
return 0;
}


//Subfunction for  "local_block_iterate3()" read a blocks of a long symlink 
static int read_syslink_block ( ext2_filsys fs, blk64_t *blocknr, e2_blkcnt_t blockcnt,
                  blk64_t /*ref_blk*/x, int /*ref_offset*/y, void *priv )
{
	char *charbuf =((struct privat*)priv)->buf;
//	__u32 nbytes;
	errcode_t retval;
//	int blocksize = fs->blocksize;

	if (*blocknr >= fs->super->s_blocks_count)
		return BLOCK_ERROR;

	if (((struct privat*)priv)->flag){
        	int allocated = block_bitmap_test(fs->block_map, *blocknr);
        	if ( allocated ){
			((struct privat*)priv)->error = 1;
//			fprintf(stderr,"Block %10lu is allocated.\n",*blocknr);
                	return (BLOCK_ABORT | BLOCK_ERROR);
		}
	}
	retval = io_channel_read_blk ( fs->io,  *blocknr,  1,  charbuf );
	if (retval){
		((struct privat*)priv)->error = retval;
		 return (BLOCK_ERROR);
	}
	if (bmap)
		block_bitmap_mark(bmap, *blocknr);
return 0;
}


//Subfunction for  "local_block_iterate3()" for recover the blocks of a real file 
static int write_block ( ext2_filsys fs, blk64_t *blocknr, e2_blkcnt_t blockcnt,
                  blk64_t /*ref_blk*/x, int /*ref_offset*/y, void *priv )
{
	int fd = ((struct privat*)priv)->fd;
        char *charbuf =((struct privat*)priv)->buf;
	__u32 nbytes;
	errcode_t retval;
	int blocksize = fs->blocksize;

	if (*blocknr >= fs->super->s_blocks_count)
		return BLOCK_ERROR;
#ifdef DEBUG
	printf("%c",(ext2fs_test_block_bitmap ( fs->block_map, *blocknr ))? 'X' : 'O');
#endif

	if (((struct privat*)priv)->flag){
        	int allocated = ext2fs_test_block_bitmap ( fs->block_map, *blocknr );
        	if ( allocated ){
//			fprintf(stderr,"Block %10lu is allocated.\n",*blocknr);
			((struct privat*)priv)->error = BLOCK_ABORT | BLOCK_ERROR ;
                	return (BLOCK_ABORT | BLOCK_ERROR);
		}
	}

	retval = io_channel_read_blk ( fs->io,  *blocknr,  1,  charbuf );
	if (retval){
		 ((struct privat*)priv)->error = BLOCK_ERROR ;
		 return (BLOCK_ERROR);
	}
	if (bmap)
		ext2fs_mark_generic_bitmap(bmap, *blocknr);

 	lseek(fd,(unsigned long long )blocksize * blockcnt, SEEK_SET);

	nbytes = write(fd, charbuf, blocksize);
        if ((unsigned) nbytes != blocksize){
		fprintf(stderr, "Error while writing file\n");
		((struct privat*)priv)->error = BLOCK_ERROR ;
        	return 8;
	}
return retval;
}


//check if the target directory existent, (recursive function)
 int check_dir(char* pathname){
	char *buffer;
	struct stat  filestat;
	char *p1;
	int retval = 1;

	if(! strlen(pathname))
		return 1;
	buffer = malloc(strlen(pathname) +3);
	strcpy(buffer,pathname);
	p1 = strrchr(buffer,'/');
	if (p1)	
		*p1 = 0;
	if (stat(buffer, &filestat) || (! S_ISDIR(filestat.st_mode)) ){ //&& (filestat.st_mode & S_IRWXU) == S_IRWXU))

	 	if (! check_dir(buffer)) {
			retval = mkdir(buffer,S_IRWXU);
		}
	}
	else 
		retval = 0;		
	if(buffer) free(buffer);		
	 return retval;
}



//subfunction for recover_file
static char* get_error_string(char* buf, int error){
	int mask = 0x80;
	int i;
	for (i = 0; i< 8 ; i++){
		*(buf+i) = (error & mask) ? '-' : 'X' ;
		mask /= 2 ;
	}
	*(buf+i) = 0;
return buf;
}



// recover Datafile
int recover_file( char* des_dir,char* pathname, char* filename, struct ext2_inode *inode, ext2_ino_t inode_nr, int flag){
	int rec_error = 255;
	char err_string[9];
	int retval =-1;
	int hardlink = 0;
	char i = 0;
	char *buf=NULL;
	struct privat priv;
	struct stat  filestat;
	char *helpname = NULL;
	char *recovername = NULL;
	char *linkname = NULL;
	char *p1;
	unsigned long long i_size;
	struct utimbuf   touchtime;
	mode_t i_mode;
	int major, minor, type;
	size_t inline_size = 0;

#ifdef DEBUG
	printf("RECOVER : INODE=%ld FILENAME=%s/%s\n",inode_nr, pathname,filename);
	dump_inode(stdout, "",inode_nr, (struct ext2_inode *)inode, 0);
#endif	
	
	if (!(inode->i_mode & LINUX_S_IFMT)) //no type flag - no recover
		return 1;

	p1 = pathname;
	while   (*p1 == '/') p1++;
	helpname = malloc(strlen(des_dir) + 15);
	recovername = malloc(strlen(des_dir) + strlen(pathname) + strlen(filename) +10);
	if (helpname && recovername){	
		strcpy(helpname,des_dir);
		strcat(helpname,"/");
		strcpy(recovername,helpname);
		strcat(recovername,p1);
		if (strlen(p1)) 
			strcat(recovername,"/");
		strcat(recovername,filename);
		while (!(stat(recovername, &filestat)) && (i<5)){
			strcat(recovername,"#");
			i++;
		}

		p1 = strchr(helpname,0);
		sprintf(p1,"#%u#",inode_nr);
		unlink (helpname);

		priv.flag = flag;

//FIXME: hardlink
		if (inode->i_links_count > 1){
			p1 = check_link_stack(inode_nr, inode->i_generation);
			if (p1) {
				//hardlink found
				if (! stat(p1, &filestat)){
					retval = check_dir(recovername);
					if (retval)
						fprintf(stderr,"Unknown error at target directory by file: %s\ntrying to continue normal\n", recovername);
					else {
						if (! link(p1,recovername)){
							if (match_link_stack(inode_nr, inode->i_generation))
								rec_error  -= CREATE_ERROR ;
							goto out;
						}
						else{	
							rec_error  -= CREATE_ERROR ;
						}
					}
				}
			}
			else{
				//flag as hardlink
				hardlink = 1;
			}
		}

	
		switch (inode->i_mode  & LINUX_S_IFMT){
//regular File;
		case LINUX_S_IFREG :
			priv.fd = open(helpname, O_WRONLY | O_CREAT | O_TRUNC | O_LARGEFILE, S_IRWXU);
			if (! priv.fd ){ 
				fprintf(stderr,"Error: open File %s for writing\n",helpname);
				retval = errno;
				goto errout;
			}
			if (inode_has_inline_data(inode)) {
				retval = read_inline_data(inode_nr, inode, &buf, &inline_size);
				if (retval) {
					close(priv.fd);
					unlink(helpname);
					retval = -1;
					goto errout;
				}
				if (write(priv.fd, buf, inline_size) != (ssize_t) inline_size) {
					close(priv.fd);
					unlink(helpname);
					retval = -1;
					goto errout;
				}
			} else {
				buf=malloc(current_fs->blocksize);
				if (!buf) {
					fprintf(stderr,"ERROR: can no allocate memory\n");
					retval = -1;
					close(priv.fd);
					goto errout;
	 			}
				priv.buf = buf;
				priv.error = 0;
				// iterate Data Blocks and if not allocated, write to file
				retval = local_block_iterate3 ( current_fs, *inode, BLOCK_FLAG_DATA_ONLY, NULL, write_block, &priv );
#ifdef DEBUG
				printf("\n");
#endif
				if (retval || priv.error){
					// error or blocks allocated , we delete the tempfile and goto out
					close(priv.fd);
					unlink(helpname);
					retval = -1;
					goto errout;
				}
				else{
				i_size = (unsigned long long)(inode->i_size | ((unsigned long long)inode->i_size_high << 32));
					retval = ftruncate(priv.fd,i_size);
					if (retval){
						rec_error -= SEEK_ERROR ;	
						}
					}
		
			}
			close(priv.fd);
		 break;

//symbolic link	
		case LINUX_S_IFLNK :
			  if (inode_has_inline_data(inode)) {
				retval = read_inline_data(inode_nr, inode, &buf, &inline_size);
				if (retval)
					goto errout;
			  }
			  else if (ext2fs_inode_data_blocks(current_fs,inode)){
				buf = malloc(current_fs->blocksize + 1); 
				if (buf) {
					memset(buf,0,current_fs->blocksize + 1);
					priv.buf = buf;
					priv.error = 0;
					
					retval = local_block_iterate3 ( current_fs, *inode, BLOCK_FLAG_DATA_ONLY, NULL, read_syslink_block, &priv );
					if (retval || priv.error)
							 goto errout;
				}
				else {
					fprintf(stderr,"ERROR: can no allocate memory\n");
					retval = -1;
					goto errout;
	 			}
			}
			else {
				int i;
			
				if((! inode->i_size) || (inode->i_size >= 60)) 
					goto errout; 
				buf = malloc(inode->i_size + 1);
				linkname = (char*) &(inode->i_block[0]);
				for (i = 0; i < inode->i_size ; i++){
					*(buf+i) = (char) *linkname;
					linkname++;
				}		 
				*(buf+i) = 0;
			}
			linkname = buf;
			retval = symlink(linkname, helpname);
			if (retval){
				rec_error  -= (CREATE_ERROR + SEEK_ERROR);
			}
		break;

//block or char device
		case LINUX_S_IFBLK :
		case LINUX_S_IFCHR :
			type = (LINUX_S_ISBLK(inode->i_mode)) ? S_IFBLK : S_IFCHR ; 

        	        if (inode->i_block[0]) {
                	        major = (inode->i_block[0] >> 8) & 255;
                        	minor = inode->i_block[0] & 255;
                	} else {
                        	major = (inode->i_block[1] & 0xfff00) >> 8;
	                        minor = ((inode->i_block[1] & 0xff) |
        	                         ((inode->i_block[1] >> 12) & 0xfff00));
                	}
			retval = mknod(helpname, type ,makedev(major, minor));
			if (retval){
				rec_error  -= (CREATE_ERROR + SEEK_ERROR);
			}
		break;

//fifo
		case LINUX_S_IFIFO:
			retval = mkfifo(helpname, S_IRWXU);
			if (retval){
				rec_error  -= (CREATE_ERROR + SEEK_ERROR);
			}
		break;

//socket and not define
		default:  
			retval = -1;
			goto errout;
		break;
	} //end switch
		

		retval = check_dir(recovername);
		if (retval)
			fprintf(stderr,"Unknown error at target directory by file: %s \ntrying to continue normal\n", recovername);
		else {
			retval = rename(helpname,recovername);
			if (retval && (errno != EEXIST)){
				rec_error -= MOVE_ERROR ;
			}

			if (! LINUX_S_ISLNK(inode->i_mode)){
				retval = chown(recovername,inode_uid(*inode), inode_gid(*inode));
				if (retval){
					rec_error -= CHOWN_ERROR ;
				}
				i_mode = inode->i_mode & 07777;
				retval = chmod(recovername,i_mode);
				if (retval){
					rec_error -= CHMOD_ERROR ;
				}
				touchtime.actime  = inode->i_atime;
				touchtime.modtime = inode->i_mtime;
				retval = utime(recovername,&touchtime);
				if (retval){
					rec_error -= UTIME_ERROR ;
				}
#ifdef FILE_ATTR
				if( LINUX_S_ISREG(inode->i_mode)){
					unsigned long flags = 0;
					if (fgetflags(recovername, &flags) == -1) {
						rec_error -= ATTRI_ERROR;
					}
					else {
						if (fsetflags(recovername, flags | (inode->i_flags & FLAGS_MODIFIABLE )) == -1 ){
							rec_error -= ATTRI_ERROR;
						}
					}		
				}
#endif
			}
			else {
				retval = lchown (recovername, inode_uid(*inode), inode_gid(*inode));
				if (retval){
					rec_error -= CHOWN_ERROR ;
				}
			}
		}
if ((hardlink) && (rec_error & SEEK_ERROR ))
	add_link_stack(inode_nr, inode->i_links_count, recovername,inode->i_generation );

out:
	printf("%s	%s\n",get_error_string(err_string,rec_error),recovername);


errout:
	if(buf) free(buf);
	if (helpname) free(helpname);
	if (recovername) free(recovername);
	
 } //helpname
return retval;
}


// check inode; return true if blocks not allocated and not recovered
int check_file_stat(struct ext2_inode *inode){
	int 				retval =-1;
	struct alloc_recover_stat	stat;

	if (!(inode->i_mode & LINUX_S_IFMT)) // no type flag
		return 0;

	stat.allocated = 0;
	stat.recovered = 0;
	if (inode_has_inline_data(inode) || (! inode->i_blocks) || (LINUX_S_ISLNK(inode->i_mode) && (inode->i_size < EXT2_N_BLOCKS*4)) ||
		 ! (ext2fs_inode_data_blocks(current_fs,inode)))
		retval = 1;
	else{
		retval = local_block_iterate3 ( current_fs, *inode, BLOCK_FLAG_DATA_ONLY, NULL, check_block_stat, &stat );
		if ( retval ) return 0;
		retval = ((! stat.allocated) && (! stat.recovered)) ? 1 : 0 ;
	}
return retval;
}


// check Datafile return the percentage of not allocated blocks
int check_file_recover(struct ext2_inode *inode, ext2_ino_t inode_nr){
	int retval =-1;
	struct alloc_stat stat;
	int use_libext2fs = 0;

	if (!(inode->i_mode & LINUX_S_IFMT)) // no type flag
		return 0;

	stat.allocated = 0;
	stat.not_allocated = 0;
	if (inode_has_inline_data(inode) || (! inode->i_blocks) || (LINUX_S_ISLNK(inode->i_mode) && (inode->i_size < EXT2_N_BLOCKS*4)) ||
		 ! (ext2fs_inode_data_blocks(current_fs,inode)))
		retval = 100;
	else{
		if (inode_nr && ext2fs_test_inode_bitmap(current_fs->inode_map, inode_nr))
			use_libext2fs = 1;
		if (use_libext2fs)
			retval = ext2fs_block_iterate3(current_fs, inode_nr, BLOCK_FLAG_DATA_ONLY,
						       NULL, check_block, &stat);
		else
			retval = local_block_iterate3(current_fs, *inode, BLOCK_FLAG_DATA_ONLY,
						      NULL, check_block, &stat);
		if ( retval ) return 0;
	
		if (stat.not_allocated)
			retval = ((stat.not_allocated * 100) / (stat.allocated + stat.not_allocated));
	}
return retval;
}



//set all attributes for directory
void set_dir_attributes(char* des_dir,char* pathname,struct ext2_inode *inode){
	char *fullname;
	char *p1;
	char err_string[9];
	int retval, rec_error = 255;
	mode_t i_mode;
	struct stat  filestat;
	struct utimbuf   touchtime;
#ifdef FILE_ATTR
	unsigned long flags = 0 ;
#endif
	fullname = malloc(strlen(des_dir) + strlen(pathname) + 3);
	if (fullname){
		p1 = pathname;
		while   (*p1 == '/') p1++;
		strcpy(fullname,des_dir);
		strcat(fullname,"/");
		strcat(fullname,p1);

		retval = check_dir(fullname);
		if ( stat(fullname, &filestat)){
			retval = mkdir(fullname,S_IRWXU);
			if (retval) 
				rec_error  -= (CREATE_ERROR + SEEK_ERROR);
		}

		retval = chown(fullname,inode_uid(*inode), inode_gid(*inode));
		if (retval){
			rec_error -= CHOWN_ERROR ;
		}

		i_mode = inode->i_mode & 07777;
		retval = chmod(fullname,i_mode);
		if (retval){
			rec_error -= CHMOD_ERROR ;
		}

		touchtime.actime  = inode->i_atime;
		touchtime.modtime = inode->i_mtime;
		retval = utime(fullname,&touchtime);
		if (retval){
			rec_error -= UTIME_ERROR ;
		}
#ifdef FILE_ATTR
		if (fgetflags(fullname, &flags) == -1) {
			rec_error -= ATTRI_ERROR;
		}
		else {
			if (fsetflags(fullname, flags | (inode->i_flags & FLAGS_MODIFIABLE )) == -1 ){
				rec_error -= ATTRI_ERROR;
			}
		}		
#endif
		printf("%s	%s\n",get_error_string(err_string,rec_error),fullname);
		
	free(fullname);
	}
}
