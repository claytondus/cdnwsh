#include "fs.h"
#include "bitmap.h"
#include "inode.h"

#define SUPERBLOCK_PADDING (BLOCK_SIZE-1048)

// free block or inode bitmap values
#define BM_FREE		0
#define BM_USED		1

#define VALID_FS 	1
#define ERROR_FS 	2

#define VFS_BLANK	0
#define VFS_GOOD	1
#define VFS_ERR		-1



typedef struct {
	uint8_t boot_record[1024]; //0
	uint32_t inode_count;      //1023
	uint32_t block_count;      //1027

	uint32_t free_inode_count; //1031
	uint32_t free_block_count; //1035

	uint32_t first_data_block; //1039, must also be the start of the directory listing for the filesystem root
	uint16_t magic; 		   //1043
	uint16_t state;			   //1045
	uint8_t padding[SUPERBLOCK_PADDING];
} superblock;


// holds values related to a virtual file system file
typedef struct {
	uint8_t state;
	superblock *superblk;
	uint8_t *free_inodes;
	uint8_t *free_blocks;
} vfs;



//********FS state and cache********

vfs fs;
block superblk_cache;
block block_bm_cache;
block inode_bm_cache;

fd_entry fd_tbl[MAX_FD];
uint8_t fd_bm[MAX_FD/8];

char cwd_str[1024];
dir_ptr* cwd;


//************flush_metadata************
void flush_metadata(void)
{
	blk_write(BLOCKID_SUPER, &superblk_cache);
	blk_write(BLOCKID_BLOCK_BITMAP, &block_bm_cache);
	blk_write(BLOCKID_INODE_BITMAP, &inode_bm_cache);
}

//*************reserve_inode************
iptr reserve_inode(void)
{
	superblock* super = (superblock*)&superblk_cache;
	if(super->free_inode_count == 0)
	{
		return 0;
	}
	iptr inode_ptr = find_free_bit(&inode_bm_cache);
	set_bitmap(&inode_bm_cache, inode_ptr);
	super->free_inode_count--;
	flush_metadata();
	return inode_ptr;
}

//**************release_inode***********
void release_inode(iptr inode_ptr)
{
	superblock* super = (superblock*)&superblk_cache;
	clear_bitmap(&inode_bm_cache, inode_ptr);
	super->free_inode_count++;
	flush_metadata();
}

//*************reserve_block************
iptr reserve_block(void)
{
	superblock* super = (superblock*)&superblk_cache;
	if(super->free_block_count == 0)
	{
		return 0;
	}
	iptr blockid = find_free_bit(&block_bm_cache);
	set_bitmap(&block_bm_cache, blockid);
	super->free_block_count--;
	flush_metadata();
	return blockid;
}

//**************release_block***********
void release_block(iptr blockid)
{
	superblock* super = (superblock*)&superblk_cache;
	clear_bitmap(&block_bm_cache, blockid);
	super->free_block_count++;
	flush_metadata();
}

//****** realloc_cache*****************
//Allocates at least "blocks_needed" blocks for this data cache
int8_t realloc_cache(fd_entry* fde, uint32_t blocks_needed)
{
	block* new_data = calloc(blocks_needed, sizeof(block));
	check(new_data != NULL, "Could not realloc data cache");
	if(fde->data != NULL) {
		memcpy(new_data, fde->data, fde->inode.size);
		free(fde->data);
	}
	fde->data = new_data;
	return 0;
error:
	return -1;
}


//****** realloc_fs_blocks*****************
//Allocates at least "blocks_needed" blocks for this file
int8_t realloc_fs_blocks(inode* inode, uint32_t blocks_needed)
{
	uint32_t* s_ind = calloc(1, sizeof(block));
	if(blocks_needed > inode->blocks)
	{
		if(inode->blocks <= 8)
		{
			for(; inode->blocks < MIN(8,blocks_needed); inode->blocks++)
			{
				inode->data0[inode->blocks] = reserve_block();
			}
		}

		if(inode->blocks < blocks_needed) //still not enough after allocating direct
		{
			//Determine if a single indirect block has been allocated
			if(inode->data1 == 0)
			{
				inode->data1 = reserve_block();
			}
			else
			{
				blk_read(inode->data1,(block*)s_ind);
			}
			//Allocate the blocks and save in the indirect block
			for(;inode->blocks < blocks_needed; inode->blocks++)
			{
				s_ind[inode->blocks-8] = reserve_block();
			}
			blk_write(inode->data1,(block*)s_ind);
		}

	}
	free(s_ind);
	return 0;
}

//****** free_fs_blocks *****************
//Frees all blocks in a inode
/*
int8_t free_fs_blocks(inode* inode)
{
	(void*)inode;
	return 0;
error:
	return -1;
}*/

//*****************mount****************
int8_t cnmount(void)
{
	blk_read(BLOCKID_SUPER, &superblk_cache);
	blk_read(BLOCKID_BLOCK_BITMAP, &block_bm_cache);
	blk_read(BLOCKID_INODE_BITMAP, &inode_bm_cache);

	fs.superblk = (superblock*)&superblk_cache;
	if(fs.superblk->magic == FS_MAGIC && fs.superblk->state == VALID_FS) {
		fs.state = VFS_GOOD;
		fs.superblk->state = ERROR_FS;
	} else
	{
		fs.state = VFS_BLANK;
	}
	memset(fd_tbl, 0, sizeof(fd_entry)*1024);
	memset(fd_bm, 0, sizeof(uint8_t)*MAX_FD/8);
	strcpy(cwd_str,"/");
	cwd = cnopendir("/");
	return 0;
}

//*****************umount****************
int8_t cnumount(void)
{
	cnclosedir(cwd);
	fs.superblk->state = VALID_FS;
	blk_write(BLOCKID_SUPER, &superblk_cache);
	blk_write(BLOCKID_BLOCK_BITMAP, &block_bm_cache);
	blk_write(BLOCKID_INODE_BITMAP, &inode_bm_cache);
	return 0;
}



//*****************mkfs****************
void superblock_init(void)
{
	superblock *sb = calloc(1,sizeof(block));
	memset(sb, 0, sizeof(block));
	sb->inode_count = INODE_COUNT;
	sb->block_count = BD_SIZE_BLOCKS;
	sb->free_inode_count = INODE_COUNT-1;
	sb->free_block_count = BD_SIZE_BLOCKS-4-INODE_TABLE_BLOCKS;  // 4 = super + bitmaps + rootdir
	sb->magic = FS_MAGIC;
	sb->state = FS_VALID;
	blk_write(BLOCKID_SUPER, (block*)sb);
	free(sb);
}

void block_bitmap_init(void)
{
	block *block_btm = calloc(1,sizeof(block));

	//Mark all reserved blocks as used
	set_bitmap(block_btm, BLOCKID_SUPER);
	set_bitmap(block_btm, BLOCKID_BLOCK_BITMAP);
	set_bitmap(block_btm, BLOCKID_INODE_BITMAP);

	//Mark all inode table blocks as used
	for (uint16_t i = BLOCKID_INODE_TABLE; i < INODE_TABLE_BLOCKS + BLOCKID_INODE_TABLE; i++) {
		set_bitmap(block_btm, i);
	}

	//Mark root directory block as used
	set_bitmap(block_btm, INODE_TABLE_BLOCKS + BLOCKID_INODE_TABLE);

	blk_write(BLOCKID_BLOCK_BITMAP, block_btm);
	free(block_btm);
}

void inode_bitmap_init(void)
{
	block *inode_btm = calloc(1,sizeof(block));

	//Mark first inode as used
	set_bitmap(inode_btm, 0);

	blk_write(BLOCKID_INODE_BITMAP, inode_btm);
	free(inode_btm);
}

void write_root_dir(void)
{
	//Prepare inode
	inode root_i;
	memset(&root_i, 0, sizeof(inode));
	uint32_t now = time(NULL);
	root_i.modified = now;
	root_i.type = ITYPE_DIR;
	root_i.size = 0;
	root_i.blocks = 1;
	root_i.data0[0] = BLOCKID_ROOT_DIR;

	block* root_dir_block = calloc(1,sizeof(block));

	// . (self entry)
	dir_entry* root_dir_entry = (dir_entry*)root_dir_block;
	root_dir_entry->inode = 0;
	root_dir_entry->file_type = ITYPE_DIR;
	root_dir_entry->name_len = 1;
	root_dir_entry->entry_len = 12;
	memcpy(root_dir_entry->name, ".", 1);
	root_i.size += 12;

	// .. (parent entry, also itself)
	root_dir_entry = (dir_entry*)(((char*)root_dir_block) + 12);
	root_dir_entry->inode = 0;
	root_dir_entry->file_type = ITYPE_DIR;
	root_dir_entry->name_len = 2;
	root_dir_entry->entry_len = 12;
	memcpy(root_dir_entry->name, "..", 2);
	root_i.size += 12;

	inode_write(0, &root_i);
	blk_write(BLOCKID_ROOT_DIR, root_dir_block);

	free(root_dir_block);
}

int8_t cnmkfs(void)
{
	superblock_init();
	block_bitmap_init();
	inode_bitmap_init();
	write_root_dir();
	return 0;
}
//******** end mkfs *****************

//******** llread *******************
//Reads a complete file from its inode data
int8_t llread(inode* inode_ptr, block* buf)
{
	uint32_t* s_ind = calloc(1, sizeof(block));
	for(uint8_t i = 0; i < MIN(inode_ptr->blocks,8); i++)
	{
		blk_read(inode_ptr->data0[i], buf);
		buf++;
	}

	if(inode_ptr->blocks > 8)
	{
		blk_read(inode_ptr->data1, (block*)s_ind);
		for(uint32_t j = 0; j < inode_ptr->blocks - 8; j++)
		{
			blk_read(s_ind[j], buf);
			buf++;
		}
	}
	free(s_ind);
	return 0;
}

//******** llwrite ******************
//Writes a file completely and updates inode data
int8_t llwrite(inode* inode_ptr, block* buf)
{
	uint32_t* s_ind = calloc(1, sizeof(block));
	for(uint8_t i = 0; i < MIN(inode_ptr->blocks,8); i++)
	{
		blk_write(inode_ptr->data0[i], buf);
		buf++;
	}

	if(inode_ptr->blocks > 8)
	{
		blk_read(inode_ptr->data1, (block*)s_ind);
		for(uint32_t j = 0; j < inode_ptr->blocks - 8; j++)
		{
			blk_write(s_ind[j], buf);
			buf++;
		}
	}
	free(s_ind);
	return 0;
}



//******** readdir ******************
//Return the dir_entry at the index within dir_ptr, and increment by entry_len.
dir_entry* cnreaddir(dir_ptr* dir)
{
	if(dir->index >= dir->inode_st.size)      //Reached the end of the directory file
	{
		return NULL;
	}
	uint8_t* entry_8 = (uint8_t*)dir->data;
	entry_8 += dir->index;
	dir_entry* entry = (dir_entry*)entry_8;
	dir->index += entry->entry_len;
	return entry;
}

//******** rewinddir *****************
void cnrewinddir(dir_ptr* dir)
{
	dir->index = 0;
}

//******** inflatedir *****************
//Populates a dir_ptr from an iptr
void inflatedir(dir_ptr* dir, iptr inode_id)
{
	inode_read(inode_id,&dir->inode_st);
	dir->inode_id = inode_id;
	//Read the directory file for this inode
	dir->data = calloc(dir->inode_st.blocks, sizeof(block));  	//Memory for all directory file blocks
	llread(&dir->inode_st, dir->data);	//Read the directory file
	dir->index = 0;
}


//******** opendir ******************
dir_ptr* cnopendir(const char* name)
{
	char name_copy[256];
	strcpy(name_copy, name);
	char* name_tok;
	char entry_name[256];
	iptr loaded_inode = INODE_COUNT + 1;		//Invalid sentinel
	dir_entry* entry;
	dir_ptr *dir = calloc(1,sizeof(dir_ptr));	//Directory file in memory (e.g. DIR object from filedef.h)

	if(strlen(name) == 0)
	{
		strcpy(name_copy,".");
	}

	if(memcmp(name,"/",1) == 0)					//Is this path absolute or relative
	{
		//Start at the root dir
		inflatedir(dir, INODE_ROOTDIR);
	}
	else
	{
		//Start at cwd
		inflatedir(dir, cwd->inode_id);
	}

	if(strcmp(name,"/") == 0)  //Root is a special case, we are done now
	{
		goto done;
	}

	name_tok = strtok(name_copy, "/");   //Guaranteed not to return NULL
	do
	{
		//Find the token in this dir
		while((entry = cnreaddir(dir)))
		{
			//Copy entry name to a null-term string to compare it
			memcpy(entry_name, entry->name, entry->name_len);
			entry_name[entry->name_len] = 0;
			if(strcmp(entry_name, name_tok) == 0)  //If this is the directory we want
			{
				loaded_inode = entry->inode;
				dir_ptr* new_dir = calloc(1, sizeof(dir_ptr));
				inflatedir(new_dir, entry->inode);
				cnclosedir(dir);  //Forget the directory we already read
				dir = new_dir;
				break;
			}
		}
		//Verify the dir was actually loaded
		check(dir->inode_id == loaded_inode, "can not find directory %s", name_copy);

		name_tok = strtok(NULL, "/");		//Read the next token

	} while(name_tok != NULL);

done:
	return dir;

error:
	free(dir);
	return NULL;
}


//******** closedir *****************
void cnclosedir(dir_ptr* dir)
{
	if(dir == NULL) return;
	if(dir->data == NULL) return;
	free(dir->data);
	free(dir);
}

//******** cd ************************
int8_t cncd(const char* name)
{
	dir_ptr* new_cwd = cnopendir(name);
	check(new_cwd != NULL, "directory %s does not exist", name);
	strcpy(cwd_str,name);
	cnclosedir(cwd);
	cwd = new_cwd;
	return 0;
error:
	return -1;
}

//******** pwd **********************
int8_t cnpwd(char* buf)
{
	strcpy(buf,cwd_str);
	return 0;
}

//******** mkdir ********************
int8_t cnmkdir(const char* name)
{
	char* name_copy = strdup(name);
	char name_tok[256];
	char entry_name[256];
	char* next_name_tok;
	dir_entry* entry;
	dir_ptr *dir = calloc(1,sizeof(dir_ptr));		//Directory file in memory (e.g. DIR object from filedef.h)
	bool last_dir = false;

	inode_read(cwd->inode_id, &dir->inode_st);		//Start at cwd
	dir->inode_id = cwd->inode_id;

	next_name_tok = strtok(name_copy, "/");
	do
	{
		//name_tok is the dir we are searching for or going to create
		strcpy(name_tok, next_name_tok);

		//Read the directory file for this inode
		dir->data = calloc(dir->inode_st.blocks,sizeof(block));  	//Memory for all directory file blocks
		llread(&dir->inode_st, dir->data);	//Read the directory file
		dir->index = 0;

		next_name_tok = strtok(NULL, "/");		//Read the next token
		if(next_name_tok == NULL)   //This is the last directory in the path
		{
			last_dir = true;
		}

		//Find the token in this dir
		while((entry = cnreaddir(dir)))
		{
			memcpy(entry_name, entry->name, entry->name_len);
			entry_name[entry->name_len] = 0;
			if(strcmp(entry_name, name_tok) == 0)  //If this directory already exists
			{
				if(last_dir)
				{
					return -1;   //Directory already exists
				}
				else   //Read the directory inode
				{
					dir->inode_id = entry->inode;
					inode_read(entry->inode,&dir->inode_st);   //Read the next directory's inode
					free(dir->data);  //Forget the directory we just read
					break;
				}
			}
		}

		if(last_dir)  //Create the directory at the end of the list
		{
			//Create parent directory entry
			entry = (dir_entry*)(((uint8_t*)dir->data)+dir->index);
			entry->file_type = ITYPE_DIR;
			entry->inode = reserve_inode();
			memcpy(entry->name,name_tok,strlen(name_tok));
			entry->name_len = strlen(name_tok);
			entry->entry_len = entry->name_len + 8;
			entry->entry_len += (4 - entry->entry_len % 4);  //padding out to 32 bits
			dir->inode_st.size += entry->entry_len;
			//TODO: handle mkdir block overflow

			//Write parent dir and inode
			dir->inode_st.modified = time(NULL);
			inode_write(dir->inode_id, &dir->inode_st);
			blk_write(dir->inode_st.data0[0], dir->data);

			//Write new directory inode
			inode new_dir_i;
			memset(&new_dir_i, 0, sizeof(inode));
			uint32_t now = time(NULL);
			new_dir_i.modified = now;
			new_dir_i.type = ITYPE_DIR;
			new_dir_i.size = 0;
			new_dir_i.blocks = 1;
			new_dir_i.data0[0] = reserve_block();

			//Write new directory file
			block* new_dir_block = calloc(1,sizeof(block));

			// . (self entry)
			dir_entry* new_dir_self_entry = (dir_entry*)new_dir_block;
			new_dir_self_entry->inode = entry->inode;
			new_dir_self_entry->file_type = ITYPE_DIR;
			new_dir_self_entry->name_len = 1;
			new_dir_self_entry->entry_len = 12;
			memcpy(new_dir_self_entry->name, ".", 1);
			new_dir_i.size += 12;

			// .. (parent entry)
			dir_entry* new_dir_parent_entry = (dir_entry*)(((uint8_t*)new_dir_block) + 12);
			new_dir_parent_entry->inode = dir->inode_id;
			new_dir_parent_entry->file_type = ITYPE_DIR;
			new_dir_parent_entry->name_len = 2;
			new_dir_parent_entry->entry_len = 12;
			memcpy(new_dir_parent_entry->name, "..", 2);
			new_dir_i.size += 12;

			//Write new dir and inode
			inode_write(entry->inode, &new_dir_i);
			blk_write(new_dir_i.data0[0], new_dir_block);

			free(new_dir_block);
			break;
		}

	} while(1);
	free(dir);
	free(name_copy);
	return 0;
}

//******** rmdir ********************
int8_t cnrmdir(const char* name)
{
	char parent_name[512];
	char entry_name[256];
	dir_entry* entry;
	inode dir_inode;
	memset(&dir_inode, 0, sizeof(inode));

	strcpy(parent_name, name);
	strcat(parent_name, "/..");

	dir_ptr* parent = cnopendir(parent_name);
	check(parent != NULL, "Cannot open parent directory");

	//Copy the entire directory minus the entry
	dir_ptr* new_parent = calloc(1,sizeof(dir_ptr));
	new_parent->data = calloc(parent->inode_st.blocks, sizeof(block));
	dir_entry* new_dir_entry = (dir_entry*)new_parent->data;
	new_parent->inode_st = parent->inode_st;
	new_parent->inode_id = parent->inode_id;
	new_parent->inode_st.size = 0;
	new_parent->index = 0;

	while((entry = cnreaddir(parent)))
	{
		memcpy(entry_name, entry->name, entry->name_len);
		entry_name[entry->name_len] = 0;
		if(strcmp(entry_name, name) == 0)  //If this is the directory we want
		{
			inode_read(entry->inode, &dir_inode);
			check(dir_inode.size == 24, "Directory is not empty");
			release_block(dir_inode.data0[0]);  //Release target directory block
			release_inode(entry->inode);        //Release target inode
			continue;
		}
		new_dir_entry->entry_len = entry->entry_len;
		new_dir_entry->name_len = entry->name_len;
		new_dir_entry->file_type = entry->file_type;
		new_dir_entry->inode = entry->inode;
		memcpy(new_dir_entry->name, entry->name, entry->name_len);
		new_parent->inode_st.size += entry->entry_len;
		new_parent->index += entry->entry_len;
		new_dir_entry = (dir_entry*)((uint8_t*)new_parent->data+new_parent->index);
	}

	inode_write(new_parent->inode_id, &new_parent->inode_st);
	llwrite(&new_parent->inode_st, new_parent->data);

	free(new_parent->data);
	free(new_parent);
	cnclosedir(parent);
	return 0;
error:
	if(new_parent != NULL && new_parent->data != NULL) free(new_parent->data);
	if(new_parent != NULL) free(new_parent);
	if(parent != NULL) cnclosedir(parent);
	return -1;
}


//******** ls ***********************
int8_t cnls(const char* name, char* buf)
{
	char name_copy[256];
	if(strlen(name) == 0)
	{
		strcpy(name_copy, ".");
	}
	else
	{
		strcpy(name_copy, name);
	}
	dir_entry* entry;
	dir_ptr* dir = cnopendir(name_copy);
	check(dir !=NULL, "can not ls directory %s", name);
	while((entry = cnreaddir(dir)))
	{
		strncat(buf, entry->name, entry->name_len);
		strcat(buf, "\n");
	}
	return 0;
error:
	return -1;
}

//******** stat *********************
int8_t cnstat(dir_ptr* dir, const char* name, stat_st *buf)
{
	char entry_name[256];
	dir_entry* entry;

	cnrewinddir(dir);
	//Find the name in this dir
	while((entry = cnreaddir(dir)))
	{
		//Copy entry name to a null-term string to compare it
		memcpy(entry_name, entry->name, entry->name_len);
		entry_name[entry->name_len] = 0;
		if(strcmp(entry_name, name) == 0)  //If this file exists
		{
			buf->inode_id = entry->inode;
			return 0;
		}
	}
	return -1;
}
//******** end stat *****************


//******** creat ********************
int8_t cncreat(dir_ptr* dir, const char* name)
{
	stat_st stat_buf;
	dir_entry* entry;

	check(cnstat(dir,name,&stat_buf) != 0, "File exists");  //If this file exists

	//Create parent directory entry
	entry = (dir_entry*)(((uint8_t*)dir->data)+dir->index);
	entry->file_type = ITYPE_FILE;
	entry->inode = reserve_inode();
	memcpy(entry->name, name, strlen(name));
	entry->name_len = strlen(name);
	entry->entry_len = entry->name_len + 8;
	entry->entry_len += (4 - entry->entry_len % 4);  //padding out to 32 bits
	dir->inode_st.size += entry->entry_len;
	//TODO: handle creat dir block overflow

	//Write parent dir and inode
	dir->inode_st.modified = time(NULL);
	inode_write(dir->inode_id, &dir->inode_st);
	blk_write(dir->inode_st.data0[0], dir->data);

	//Write new file inode
	inode new_file_i;
	memset(&new_file_i, 0, sizeof(inode));
	uint32_t now = time(NULL);
	new_file_i.modified = now;
	new_file_i.type = ITYPE_FILE;
	new_file_i.size = 0;
	new_file_i.blocks = 0;
	inode_write(entry->inode, &new_file_i);

	return 0;

error:
	return -1;
}

//******** cnopen *********************
//mode: FD_READ/FD_WRITE
int16_t cnopen(dir_ptr* dir, const char* name, uint8_t mode)
{
	stat_st stat_buf;
	if(cnstat(dir,name,&stat_buf) != 0)
	{
		if(mode == FD_WRITE) {
			check(cncreat(dir,name) == 0, "Could not create %s", name);
		}
		check(cnstat(dir,name,&stat_buf) == 0, "Could not stat %s", name);
	}
	//TODO: The fd bitmap is not 1 block long, hope we don't run out of fds
	int16_t fd = (int16_t)(uint16_t)find_free_bit((block*)fd_bm);
	set_bitmap((block*)fd_bm, fd);
	fd_tbl[fd].cursor = 0;
	fd_tbl[fd].state = mode;
	fd_tbl[fd].inode_id = stat_buf.inode_id;
	inode_read(stat_buf.inode_id, &fd_tbl[fd].inode);

	if(fd_tbl[fd].inode.blocks > 0)
	{
		fd_tbl[fd].data = calloc(fd_tbl[fd].inode.blocks,sizeof(block));
		llread(&fd_tbl[fd].inode, fd_tbl[fd].data);
	}
	else
	{
		fd_tbl[fd].data = NULL;
	}
	return fd;

error:
	return -1;
}
//******** end open *****************


//******** cnclose *********************
int8_t cnclose(int16_t fd)
{
	if(fd_tbl[fd].state == FD_FREE)
	{
		return -1;
	}
	if(fd_tbl[fd].data != NULL)
	{
		free(fd_tbl[fd].data);
	}
	clear_bitmap((block*)fd_bm, fd);
	fd_tbl[fd].state = FD_FREE;
	return 0;
}

//******** cnread ********************
size_t cnread(uint8_t* buf, size_t bytes, int16_t fd)
{
	size_t bytes_to_read = 0;
	fd_entry* fde = &fd_tbl[fd];
	check(fde->state == FD_READ, "File descriptor not in read mode");
	//TODO: Don't read unallocated space
	if(bytes > (fde->inode.size - (fde->cursor + 1)))
	{
		bytes_to_read = fde->inode.size - (fde->cursor+1);
	}
	else
	{
		bytes_to_read = bytes;
	}
	uint8_t* data_ptr = ((uint8_t*)fde->data) + fde->cursor;
	memcpy(buf, data_ptr, bytes_to_read);
	fde->cursor += bytes_to_read;
	return bytes_to_read;
error:
	return 0;
}


//****** cnseek **********************
int8_t cnseek(int16_t fd, uint32_t offset)
{
	fd_entry* fde = &fd_tbl[fd];
	uint32_t required_size = fde->cursor + offset;
	if(fde->inode.size < required_size)   //The data cache and block size may have to be expanded
	{
		//Compute new sizes
		uint32_t required_blocks = (required_size / BLOCK_SIZE) + 1;

		//Allocate new data cache
		check(realloc_cache(fde, required_blocks) == 0, "Unable to realloc cache");
		fde->inode.size = required_size;

		//Allocate fs blocks
		if(fde->inode.blocks < required_blocks)
		{
			check(realloc_fs_blocks(&fde->inode, required_blocks) == 0, "Could not allocate fs blocks");
		}
		fde->inode.modified = time(NULL);
		inode_write(fde->inode_id, &fde->inode);
	}
	fde->cursor = offset;
	return 0;
error:
	return -1;
}


//****** cnwrite *********************
size_t cnwrite(uint8_t* buf, size_t bytes, int16_t fd)
{
	fd_entry* fde = &fd_tbl[fd];
	check(fde->state == FD_WRITE, "File descriptor not in write mode");

	uint32_t required_size = fde->cursor + bytes;
	if(fde->inode.size < required_size)   //The data cache and block size may have to be expanded
	{
		//Compute new sizes
		uint32_t required_blocks = (required_size / BLOCK_SIZE) + 1;

		//Allocate new data cache
		check(realloc_cache(fde, required_blocks) == 0, "Unable to realloc cache");
		fde->inode.size = required_size;

		//Allocate fs blocks
		if(fde->inode.blocks < required_blocks)
		{
			check(realloc_fs_blocks(&fde->inode, required_blocks) == 0, "Could not allocate fs blocks");
		}
		fde->inode.modified = time(NULL);
		inode_write(fde->inode_id, &fde->inode);
	}

	uint8_t* data_ptr = ((uint8_t*)fde->data)+fde->cursor;
	memcpy(data_ptr, buf, bytes);
	fde->cursor += bytes;
	llwrite(&fde->inode, (block*)fde->data);

	return bytes;
error:
	return 0;
}

//****** cncat *********************
int8_t cncat(const char* name, char* buf)
{
	stat_st filestat;
	dir_ptr* dir = cnopendir(".");

	check(cnstat(dir, name, &filestat) == 0, "Can not stat file");
	inode file_i;
	inode_read(filestat.inode_id, &file_i);

	int8_t fd = cnopen(dir,name,FD_READ);
	check(fd >= 0, "Can not open file");
	cnread((uint8_t*)buf, file_i.size, fd);
	return 0;
error:
	cnclosedir(dir);
	return -1;
}

//****** cnimport ****************
int8_t cnimport(const char* h_name, const char* g_name)
{
	FILE* h_file;
	size_t h_size;
	char* buf;
	size_t result;
	int16_t g_file;

	h_file = fopen(h_name, "rb");
	check(h_file != NULL, "Can not open host file");

	//get file size
	fseek(h_file, 0, SEEK_END);
	h_size = ftell(h_file);
	rewind(h_file);

	dir_ptr* cwd = cnopendir(".");
	check(cncreat(cwd, g_name) == 0, "Cannot creat guest file");
	g_file = cnopen(cwd, g_name, FD_WRITE);
	check(g_file >= 0, "Cannot open guest file for writing");

	//buffer for whole file
	buf = calloc(h_size, sizeof(char));

	//load file
	result = fread(buf,1,h_size,h_file);
	check(result == (size_t)h_size, "Error reading from host file");

	//write the buffer to the guest
	result = cnwrite((uint8_t*)buf, h_size, g_file);
	check(result > 0, "Error writing to guest file");

	//close guest resources
	cnclose(g_file);
	cnclosedir(cwd);
	fclose(h_file);
	free(buf);
	return 0;
error:
	//close guest resources
	cnclose(g_file);
	cnclosedir(cwd);
	fclose(h_file);
	free(buf);
	return -1;
}


//****** cnexport ****************
int8_t cnexport(const char* g_name, const char* h_name)
{
	FILE* h_file;
	size_t h_size;
	char* buf;
	size_t result;
	int16_t g_file;
	stat_st statbuf;

	h_file = fopen(h_name, "wb");
	check(h_file != NULL, "Can not open host file");

	dir_ptr* cwd = cnopendir(".");
	g_file = cnopen(cwd, g_name, FD_READ);
	check(g_file >= 0, "Cannot open guest file for reading");

	check(cnstat(cwd, g_name, &statbuf) == 0, "Cannot stat guest file");
	inode g_inode;
	inode_read(statbuf.inode_id, &g_inode);
	h_size = g_inode.size;

	//buffer for whole file
	buf = calloc(h_size, sizeof(char));

	//load file
	result = cnread((uint8_t*)buf, h_size, g_file);
	check(result == h_size, "Error reading from guest file");

	//write the buffer to the host
	result = fwrite(buf, sizeof(char), h_size, h_file);
	check(result > 0, "Error writing to host file");

	//close guest resources
	cnclose(g_file);
	cnclosedir(cwd);
	fclose(h_file);
	free(buf);
	return 0;
error:
	//close guest resources
	cnclose(g_file);
	cnclosedir(cwd);
	fclose(h_file);
	free(buf);
	return -1;
}

//******* cntree ************
void treedir(dir_ptr* dir, uint8_t indents, char** buf)
{
	dir_entry* entry;
	inode entry_i;
	memset(&entry_i, 0, sizeof(inode));
	int chars;

	//Write indent pattern
	char indent_str[256];
	uint8_t indents_ptr = indents;
	while(indents_ptr > 0)
	{
		strcat(indent_str,"    ");
		indents_ptr--;
	}

	while((entry = cnreaddir(dir)))
	{
		char name_copy[256];
		memset(name_copy, 0, 256);
		memcpy(name_copy, entry->name, entry->name_len);
		if(strcmp(name_copy,".") == 0) continue;
		if(strcmp(name_copy,"..") == 0) continue;

		inode_read(entry->inode,&entry_i);

		if(indents > 0)
		{
			strcpy(*buf,indent_str);
			*buf += strlen(indent_str);
		}
		memcpy(*buf, entry->name, entry->name_len);
		*buf += entry->name_len;
		chars = sprintf(*buf, "  %s  %u  %s", (entry_i.type == ITYPE_FILE) ? "F":"D", entry_i.size, ctime((const time_t *)&entry_i.modified));
		*buf += chars;

		if(entry->file_type == ITYPE_DIR)  //Go to next indent level if a dir
		{
			cncd(name_copy);
			dir_ptr* next_dir = cnopendir(".");
			treedir(next_dir,indents+1,buf);
			cnclosedir(next_dir);
			cncd("..");
		}
	}
}

int8_t cntree(char* buf)
{
	dir_ptr* dir = cnopendir(".");
	uint8_t indents = 0;
	treedir(dir,indents,&buf);
	return 0;
}
