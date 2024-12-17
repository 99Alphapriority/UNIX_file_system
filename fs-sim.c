#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <sys/mman.h>

#include "fs-sim.h"

#define FREE_SPACE_SIZE		16 //bytes
#define INODE_COUNT			126
#define INODE_LIST_SIZE		sizeof(Inode) * INODE_COUNT
#define ROOT_DIR			127
#define DATA_BLOCK_COUNT	127
#define DATA_BLOCK_SIZE		1024

int diskFD = -1;
int mountedDiskFD = -1;
Superblock *temp_superBlock = NULL;
Superblock *superBlock = NULL;
uint8_t cwd = 0;
char diskName[10];
char buffer[1024] = {'\0'};

void writeSuperBlock()
{
	lseek(mountedDiskFD, 0, SEEK_SET);

	//wite the free_block_list and inode list to the memory
	write(mountedDiskFD, superBlock->free_block_list, FREE_SPACE_SIZE);
	write(mountedDiskFD, superBlock->inode, sizeof(Inode) * INODE_COUNT);
}

void fs_resize(char name[5], int new_size)
{
	
	int inodeIdx = -1;

	for(int i = 0; i < INODE_COUNT; i++)
	{
		if((superBlock->inode[i].used_size & 0x80) && (0 == strncmp(name, superBlock->inode[i].name,5) && 
					(cwd == (superBlock->inode[i].dir_parent & 0x7F))))
		{
			inodeIdx = i;
			break;
		}
	}

	if(-1 == inodeIdx)
		fprintf(stderr,"File %s does not exist\n",name);

	int startBlock = superBlock->inode[inodeIdx].start_block;
    int oldSize = superBlock->inode[inodeIdx].used_size & 0x7F;

    lseek(mountedDiskFD, 0, SEEK_SET);

	if(new_size < oldSize)
	{
		for(int i = startBlock + new_size; i< startBlock + oldSize; i++)
		{
			void *filePtr = mmap(NULL, 128 * DATA_BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mountedDiskFD, 0);

            memset(filePtr + (i * DATA_BLOCK_SIZE), 0, DATA_BLOCK_SIZE);
            superBlock->free_block_list [i/8] &= ~(1 << (7 - (i%8)));
		}

		superBlock->inode[inodeIdx].used_size = 0x80 | new_size;
	}
	else
	{
		bool contiguousBlocks = true;
		//check if contiguous data blocks are available from the current last data block
		for(int i = startBlock + oldSize; i < startBlock + new_size; i++)
		{
			if(superBlock->free_block_list[i/8] & (1 << (7 - (i%8))))
				contiguousBlocks = false;
		}

		if(contiguousBlocks)
		{
			//mark data blocks as used and update the inode values
			for(int i = startBlock + oldSize; i < startBlock + new_size; i++)
			{
				superBlock->free_block_list [i/8] |= (1 << (7 - (i%8)));
			}
			superBlock->inode[inodeIdx].used_size = 0x80 | new_size;

		}
		else
		{
			contiguousBlocks = false;
			int newStartBlock = -1;
			//find continguous blocks
			for(int i = 0; i < DATA_BLOCK_COUNT; i++)
			{
				if(-1 != newStartBlock)
				{
					contiguousBlocks = true;
					break;
				}

				if(superBlock->free_block_list[i/8] & (1 << (7 - (i%8))))
					continue;

				for(int j = i+1; j < DATA_BLOCK_COUNT; j++)
				{
					if(new_size == j-i+1)
					{	
						newStartBlock = i;
						break;
					}

					if(superBlock->free_block_list[j/8] & (1 << (7 - (j%8))))
					{
						i = j;
						break;
					}
				}
			}

			if(!contiguousBlocks)
			{
				//this means there aren't enough contiguous free blocks
				fprintf(stderr,"Error: File %s cannot expand to size %d\n",name, new_size);
			}
			else
			{
				//move the data blocks to new location and empty out old data blocks
				for(int j = startBlock, i = newStartBlock; j < startBlock + oldSize; j++,i++)
				{

					void *filePtr = mmap(NULL, 128 * DATA_BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mountedDiskFD, 0);

					memcpy(filePtr + (i * DATA_BLOCK_SIZE), filePtr + (j * DATA_BLOCK_SIZE), DATA_BLOCK_SIZE);
					memset(filePtr + (j * DATA_BLOCK_SIZE), 0, DATA_BLOCK_SIZE);
	
					//mark ith data block as used and jth data block as empty
					superBlock->free_block_list [i/8] |= (1 << (7 - (i%8)));
					superBlock->free_block_list [j/8] &= ~(1 << (7 - (j%8)));
				}

				//mark the remaining data blocks as used
				for(int i = newStartBlock + oldSize; i < newStartBlock + new_size; i++)
					superBlock->free_block_list [i/8] |= (1 << (7 - (i%8)));

				//update the inode values
				superBlock->inode[inodeIdx].start_block = newStartBlock;
				superBlock->inode[inodeIdx].used_size = 0x80 | new_size;
			}
		}
	}
	writeSuperBlock();
}

void fs_defrag(void)
{
	int newStartBlock = 0;
	for(int i = 0; i <= DATA_BLOCK_COUNT; i++)
	{
		int startBlock = 0;
		int fileSize = 0;
		int inodeIdx = 0;

		//check if the ith data block is in use or not.
		if(superBlock->free_block_list[i/8] & (1 << (7 - (i%8))))
			continue;

		newStartBlock = i;
		//If a free data block is encountered, iterate to the next used data block.
		//this data block will be the start_block of the file
		for(int j = i + 1; j <= DATA_BLOCK_COUNT; j++)
		{
			if(DATA_BLOCK_COUNT == j && !(superBlock->free_block_list[j/8] & (1 << (7 - (j%8)))))
			{
				writeSuperBlock();
				return;
			}
			//If the data block is empty keep moving to the next data block
			if(!(superBlock->free_block_list[j/8] & (1 << (7 - (j%8)))))
				continue;
			startBlock = j;
			break;
		}

		//Find the inode corresponding to this data block
		for(int j = 0; j < INODE_COUNT; j++)
		{
			if(startBlock == superBlock->inode[j].start_block)
			{
				inodeIdx = j;
				break;
			}
		}

		fileSize = superBlock->inode[inodeIdx].used_size & 0x7F;

		//At this point we have the start_block and size of the file.
		//i -> index of the lowest free data block.
		
		//reset the disk FD back to the beginning of the disk in order to move data 
		//from one data block to another
		lseek(mountedDiskFD, 0, SEEK_SET);
		superBlock->inode[inodeIdx].start_block = newStartBlock;

		for(int j = startBlock; j < startBlock + fileSize; j++)
		{
			void *filePtr = mmap(NULL, 128 * DATA_BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mountedDiskFD, 0);

			memcpy(filePtr + (i * DATA_BLOCK_SIZE), filePtr + (j * DATA_BLOCK_SIZE), DATA_BLOCK_SIZE);
			memset(filePtr + (j * DATA_BLOCK_SIZE), 0, DATA_BLOCK_SIZE);

			//mark ith data block as used and jth data block as empty
			superBlock->free_block_list [i/8] |= (1 << (7 - (i%8)));
			superBlock->free_block_list [j/8] &= ~(1 << (7 - (j%8)));

			if(j != startBlock + fileSize - 1)
				i++;
		}

		//currently i is at the original last data block of the file pointed by
		//inode[inodeIdx].
		
		//Update the new startBlock value in the inode
		//superBlock->inode[inodeIdx].start_block = newStartBlock;
	}

	//writeSuperBlock();
}

void fs_cd(char name[5])
{
	if(0 == strcmp(name,"."))
		return;
	if(0 == strcmp(name,".."))
	{
		if(ROOT_DIR == cwd)
			return;
		cwd = superBlock->inode[cwd].dir_parent & 0x7F;
		return;
	}

	int inodeIdx = -1;
	for(int i = 0; i < INODE_COUNT; i++)
	{
		if((superBlock->inode[i].used_size & 0x80) && (0 == strncmp(superBlock->inode[i].name,name,5)) &&
			(cwd == (superBlock->inode[i].dir_parent & 0x7F)))
		{
			inodeIdx = i;
			break;
		}
	}

	if((-1 == inodeIdx) || !(superBlock->inode[inodeIdx].dir_parent & 0x80))
	{
		fprintf(stderr,"Error: Directory %s does not exist\n", name);
		return;
	}

	cwd = inodeIdx;
}

void fs_read(char name[5], int block_num)
{
	int inodeIdx = -1;
	for(int i = 0; i < INODE_COUNT; i++)
	{
		if((superBlock->inode[i].used_size & 0x80) && (0 == strncmp(superBlock->inode[i].name, name, 5)))
		{
			inodeIdx = i;
			break;
		}
	}

	if(-1 == inodeIdx || superBlock->inode[inodeIdx].dir_parent & 0x80)
	{
		fprintf(stderr,"Error: File %s does not exist\n", name);
		return;
	}

	uint8_t startBlock = superBlock->inode[inodeIdx].start_block;
	uint8_t size = superBlock->inode[inodeIdx].used_size & 0x7F;

	if (block_num < 0 || block_num > size)
	{
		fprintf(stderr,"Error: %s does not have block %d\n", name, block_num);
		return;
	}
	
	uint8_t readBlock = startBlock + block_num;

	lseek(mountedDiskFD, readBlock * DATA_BLOCK_SIZE, SEEK_SET);
	memset(buffer, '\0', DATA_BLOCK_SIZE);
	read(mountedDiskFD, buffer, DATA_BLOCK_SIZE);

}


void fs_delete(char name[5], int directory)
{
	if(-1 == mountedDiskFD)
    {
        fprintf(stderr,"Error: No file system is mounted\n");
        return;
    }

	int inodeIdx = -1;
	for(int i = 0; i < INODE_COUNT; i++)
	{
		if((superBlock->inode[i].used_size & 0x80) && (0 == strncmp(superBlock->inode[i].name, name, 5))
				&& (directory == (superBlock->inode[i].dir_parent & 0x7F)))
		{	
			inodeIdx = i;
			break;
		}
	}

	if(-1 == inodeIdx)
	{
		fprintf(stderr,"Error: File or directory %s does not exist\n", name);
		return;
	}

	//If it is a directory then recursively delete the contents of the directory
	if(superBlock->inode[inodeIdx].dir_parent & 0x80)
	{
		for(int i = 0; i < INODE_COUNT; i++)
		{
			if((superBlock->inode[i].used_size & 0x80) && ((superBlock->inode[i].dir_parent & 0x7F) == (inodeIdx)))
			{
				fs_delete(superBlock->inode[i].name, (superBlock->inode[i].dir_parent & 0x7F));
			}
		}
	}

	int startBlock = superBlock->inode[inodeIdx].start_block;
	int size = superBlock->inode[inodeIdx].used_size & 0x7F;

	//Delete the data blocks used by the file
	for(int i = startBlock; i < startBlock + size; i++)
	{
		void *filePtr = mmap(NULL, 128 * DATA_BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mountedDiskFD, 0);
		
		memset(filePtr + (i * DATA_BLOCK_SIZE), 0, DATA_BLOCK_SIZE);

		superBlock->free_block_list[i/8] &= ~(1 << (7 - (i%8)));
	}

	memset(&superBlock->inode[inodeIdx], 0, sizeof(Inode));
	writeSuperBlock();

}

void fs_ls(void)
{
	if(-1 == mountedDiskFD)
	{
        fprintf(stderr,"Error: No file system is mounted\n");
        return;
    }

	int currDirCount = 0;
	int prevDirCount = 0;

	for(int i = 0; i < INODE_COUNT; i++)
	{
		if((superBlock->inode[i].used_size & 0x80) && (superBlock->inode[i].dir_parent & 0x7F) == cwd)
		{
			currDirCount++;
		}
	}

	if(ROOT_DIR != cwd)
	{
		int parentDir = superBlock->inode[cwd].dir_parent & 0x7F;
		for(int i = 0; i < INODE_COUNT; i++)
		{
			if((superBlock->inode[i].used_size & 0x80) && (superBlock->inode[i].dir_parent & 0x7F) == parentDir)
			{
				prevDirCount++;
			}
		}
	}

	fprintf(stdout,".     %3d\n", currDirCount + 2);
	fprintf(stdout,"..    %3d\n", (ROOT_DIR == cwd) ? currDirCount + 2 : prevDirCount + 2);

	for(int i = 0; i < INODE_COUNT; i++)
	{
		prevDirCount = 0;
		if((superBlock->inode[i].used_size & 0x80) && (superBlock->inode[i].dir_parent & 0x7F) == cwd)
		{
			if(superBlock->inode[i].dir_parent & 0x80)
			{
				int currParentDir = i;
				for(int j = 0; j < INODE_COUNT; j++)
				{
					if((superBlock->inode[j].used_size & 0x80) && (superBlock->inode[j].dir_parent & 0x7F) == currParentDir)
					{
						prevDirCount++;
					}
				}
				for(int j = 0; j < 5; j++)
				{
					if(superBlock->inode[i].name[j] != '\0')
						fprintf(stdout,"%c",superBlock->inode[i].name[j]);
					else
						fprintf(stdout," ");
				}
					fprintf(stdout," %3d\n", prevDirCount + 2);
			}
			else
			{
				for(int j = 0; j < 5; j++)
                {
                    if(superBlock->inode[i].name[j] != '\0')
                        fprintf(stdout,"%c",superBlock->inode[i].name[j]);
                    else
                        fprintf(stdout," ");
                }
                    fprintf(stdout," %3d KB\n", superBlock->inode[i].used_size & 0x7f);
			}
		}
	}
}

void fs_write(char name[5], int block_num)
{
	//check if the file with the given name exists
	int inodeIdx = -1;
	for(int i = 0; i < INODE_COUNT; i++)
	{
		if((superBlock->inode[i].used_size & 0x80) && (0 == strncmp(superBlock->inode[i].name, name, 5))
				&& (cwd == (superBlock->inode[i].dir_parent & 0x7F)))
		{
			inodeIdx = i;
			break;
		}
	}

	if(-1 == inodeIdx || superBlock->inode[inodeIdx].dir_parent & 0x80)
	{
		fprintf(stderr,"Error: File %s does not exist\n",name);
		return;
	}

	//check if block_num is in range
	int size = superBlock->inode[inodeIdx].used_size & 0x7F;
	if(0 > block_num || block_num >= size)
	{
		fprintf(stderr,"Error: %s does not have block %d\n", name, block_num);
		return;
	}

	//write the buffer to the file
	int startBlock = superBlock->inode[inodeIdx].start_block;
	int writeBlock = startBlock + block_num;

	lseek(mountedDiskFD, writeBlock * DATA_BLOCK_SIZE, SEEK_SET);
	write(mountedDiskFD, buffer, DATA_BLOCK_SIZE);

}

void fs_buff(char buff[1024])
{
	if(-1 == mountedDiskFD)
	{
		fprintf(stderr,"Error: No file system is mounted\n");
		return;
	}

	memset(buffer, '\0', sizeof(buffer));

	for(int i = 0; (i < 1024) && (buff[i] != '\0'); i++)
		buffer[i] = buff[i];
}

void fs_create(char name[5], int size)
{

	if(-1 == mountedDiskFD)
	{
        fprintf(stderr,"Error: No file system is mounted\n");
        return;
    }


	int free_inode_idx = -1;

	//check for a free inode
	for(int i = 0; i < INODE_COUNT; i++)
	{
		if(!(superBlock->inode[i].used_size & 0x80))
		{
			free_inode_idx = i;
			break;
		}
	}

	if(-1 == free_inode_idx)
	{	
		fprintf(stderr,"Error: Superblock in disk %s is full, cannot create %s\n", diskName, name);
		return;
	}

	//check if the file or directory name is unique in the curernt working directory
	for(int i = 0; i < INODE_COUNT; i++)
	{
		if((superBlock->inode[i].used_size & 0x80) && 
				(superBlock->inode[i].dir_parent == cwd) && 
				(0 == strncmp(superBlock->inode[i].name, name, 5)))
		{
			fprintf(stderr,"Error: File or directory %s already exists\n", name);
			return;
		}
	}

	//check if contiguous blocks are available
	int start_block = -1;
	if (0 < size)
	{
		int free_block_count = 0;

		for(int i = 1; i <= DATA_BLOCK_COUNT; i++)
		{
			if(!(superBlock->free_block_list[i/8] & (1 << (7 - (i%8)))))
			{
				free_block_count++;
				if(free_block_count == size)
				{
					start_block = i - size + 1;
					break;
				}
			}
		else
			free_block_count = 0; //reset
		}

		if(-1 == start_block)
		{
			fprintf(stderr,"Error: Cannot allocate %d blocks on %s\n", size, diskName);
			return;
		}

		//mark data blocks as allocated
		for(int i = start_block; i < start_block + size; i++)
			superBlock->free_block_list[i/8] |= (1 << (7 - (i%8)));
	}

	strncpy(superBlock->inode[free_inode_idx].name, name, 5);

	//populate the inode parameters
	//Mark the 8th bit as used and set the remaining bits as the size of the file
	superBlock->inode[free_inode_idx].used_size = 0x80 | size;

	//Mark the start_block as 0 if it is a directory
	superBlock->inode[free_inode_idx].start_block = (size > 0) ? start_block : 0;

	//If it is a directory mark the 8th bit as 1 and set the remaining bits to the CWD
	superBlock->inode[free_inode_idx].dir_parent = (size == 0) ? 0x80 | cwd : cwd;
	writeSuperBlock();

}

int inodeConsistencyCheck(char *diskName)
{
	for(int i = 0; i < INODE_COUNT; i++)
	{
		Inode *inode = &temp_superBlock->inode[i];

		//check inode state
		if(!(inode->used_size & 0x80))
		{
			//if inode state is free then the contents should also be 0 or empty

			if(inode->name[0] != 0 || inode->start_block != 0|| inode->dir_parent != 0)
			{
				fprintf(stderr,"Error: File system in %s is inconsistent (error code: 1)\n",diskName);
				return 1;
			}
		}
		else
		{
			if(0 == strlen(inode->name))
			{
				fprintf(stderr,"Error: File system in %s is inconsistent (error code: 1)\n",diskName);
				return 1;
			}
		}

		//consistency check if inode pertains to a file
		if((inode->used_size & 0x80) && (inode->used_size & 0x7F) && !(inode->dir_parent & 0x80))
		{
			int fileSize = inode->used_size & 0x7F;
			int startBlock = inode->start_block;

			if(startBlock < 1 || startBlock > 127 || startBlock + fileSize - 1 > 127)
			{
				fprintf(stderr,"Error: File system in %s is inconsistent (error code: 2)\n",diskName);
                return 1;
			}
		}
		//consistency check if inode pertains to a directory
		else if ((inode->used_size & 0x80) && (inode->dir_parent & 0x80))
		{
			if((inode->start_block != 0 || (inode->used_size > 128)))
			{
				fprintf(stderr,"Error: File system in %s is inconsistent (error code: 3)\n",diskName);
                return 1;
            }
		}
		
		//check parent directory attributes
		int parentIndex = inode->dir_parent & 0x7F;

		if((inode->used_size & 0x80) && parentIndex > 0 && 126 > parentIndex)
		{
			if(!(temp_superBlock->inode[parentIndex].used_size & 0x80))
			{
				fprintf(stderr,"Error: File system in %s is inconsistent (error code: 4)\n",diskName);
				return 1;
			}

			if(!(temp_superBlock->inode[parentIndex].dir_parent & 0x80))
			{
				fprintf(stderr,"Error: File system in %s is inconsistent (error code: 4)\n",diskName);
                return 1;
			}
		}
		else if ((inode->used_size & 0x80) && 126 == parentIndex)
		{
			fprintf(stderr,"Error: File system in %s is inconsistent (error code: 4)\n",diskName);
            return 1;
		}
	}

	//Check if every file/directory is unique in every directory
	for(int i = 0; i < INODE_COUNT; i++)
	{
		if(temp_superBlock->inode[i].used_size & 0x80)
		{
			for(int j = i+1; j < INODE_COUNT; j++)
			{
				if((temp_superBlock->inode[j].used_size & 0x80) && 
						0 == strncmp(temp_superBlock->inode[j].name, temp_superBlock->inode[i].name,5) && 
						temp_superBlock->inode[i].dir_parent == temp_superBlock->inode[j].dir_parent)
				{
					fprintf(stderr,"Error: File system in %s is inconsistent (error code: 5)\n",diskName);
					return 1;
				}
			}
		}
	}

	//Validate free space list. 
	char temp_free_block_list[FREE_SPACE_SIZE] = {0};
	//The first bit of the free_block_list is set to 1 as the superblock is in use
	temp_free_block_list[0] |= (1 << 7); 
	
	for(int i =  0; i < INODE_COUNT; i++)
	{
		if(temp_superBlock->inode[i].used_size & 0x80)
		{
			int size = temp_superBlock->inode[i].used_size & 0x7F;
			int startBlock = temp_superBlock->inode[i].start_block;

			for(int j = 0; j < size; j++)
			{
				int blockIdx = startBlock + j;

				//This will set the bits corresponding to the data blocks occupied by the file
				temp_free_block_list[blockIdx/8] |= (1 << (7 - (blockIdx % 8)));
			}
		}
	}

	//Check if the free list created using the inodes is same as the actual free 
	//space list stored on the disk
	if(0 != memcmp(temp_free_block_list, temp_superBlock->free_block_list, FREE_SPACE_SIZE))
	{
		fprintf(stderr,"Error: File system in %s is inconsistent (error code: 6)\n", diskName);
		return -1;
	}

	for(int i = 0 ; i < INODE_COUNT; i++)
	{
		if(!(temp_superBlock->inode[i].used_size & 0x80))
			continue;

		int startBlock = temp_superBlock->inode[i].start_block;
		//int endBlock = startBlock + (temp_superBlock->inode[i].used_size & 0x7F) - 1;

		for(int j = i + 1; j < INODE_COUNT; j++)
		{
			if(!(temp_superBlock->inode[j].used_size & 0x80))
				continue;
			if(temp_superBlock->inode[j].start_block == startBlock && 
					(temp_superBlock->inode[j].used_size & 0x7F) == (temp_superBlock->inode[i].used_size & 0x7F))
			{
				fprintf(stderr,"Error: File system in %s is inconsistent (error code: 6)\n", diskName);
				return -1;
			}
#if 0
			if((temp_superBlock->inode[j].start_block) > startBlock && (endBlock > temp_superBlock->inode[j].start_block))
			{
				fprintf(stderr,"Error: File system in %s is inconsistent (error code: 6)\n", diskName);
				return -1;
			}

			if(temp_superBlock->inode[j].start_block < startBlock && 
					(endBlock < (temp_superBlock->inode[j].start_block + 
								 (temp_superBlock->inode[j].used_size & 0x7F) - 1)))
			{
				fprintf(stderr,"Error: File system in %s is inconsistent (error code: 6)\n", diskName);
                return -1;
			}
#endif
		}
	}

	return 0;
}

void fs_mount(char *new_disk_name)
{
	//Create a temporary superBlock that will check the consistency of the disk 
	//to be mounted
	temp_superBlock = (Superblock*) malloc (sizeof(Superblock));
	memset(temp_superBlock, 0, sizeof(Superblock));

	diskFD = open(new_disk_name, O_RDWR);

	if(0 > diskFD)
	{
		fprintf(stderr,"Error: Cannot find disk %s\n", new_disk_name);
		free(temp_superBlock);
		return;
	}

	//reset the diskFD to the start of the virtual disk
	lseek(diskFD, 0, SEEK_SET);

	//load the free_block_list from the superblock
	if(FREE_SPACE_SIZE != read(diskFD, temp_superBlock->free_block_list, FREE_SPACE_SIZE))
	{
		fprintf(stderr,"Error: Cannot read the superblock\n");
		close(diskFD);
		diskFD = -1;
		free(temp_superBlock);
		return;
	}

	//load the inode array from the superblock
	if(INODE_LIST_SIZE != read(diskFD, temp_superBlock->inode, INODE_LIST_SIZE * sizeof(char)))
	{
		fprintf(stderr,"Error: Cannot read inodes\n");
		close(diskFD);
		diskFD = -1;
		free(temp_superBlock);
		return;
	}

	//inode consistency check
	if(inodeConsistencyCheck(new_disk_name))
	{
		close(diskFD);
		diskFD = -1;
		free(temp_superBlock);
		return;
	}
	
	//reset the diskFD to the beginning of the disk
	lseek(diskFD, 0, SEEK_SET);

	//Update the mounted disk FD
	mountedDiskFD = diskFD;
	cwd = ROOT_DIR;

	strcpy(diskName, new_disk_name);
	//Transfer the contents from the temporary super block to the global super block
	//structure
	memcpy(superBlock, temp_superBlock, sizeof(Superblock));
	memset(temp_superBlock, 0, sizeof(Superblock));
	free(temp_superBlock);
	writeSuperBlock();

}

int main(int argc, char **argv)
{
	if (2 != argc)
	{
		fprintf(stderr,"Usage: %s <input_file>\n",argv[0]);
		return 1;
	}

	FILE *inputFile = fopen(argv[1], "r");

	if (NULL == inputFile)
	{
		perror("Error in opening the input file");
		return 1;
	}

	char command;
	char line[1024];
	char num[3];
	char arg1[5] = {'\0'};
	int arg2;
	int lineNum = 0;

	superBlock = (Superblock*) malloc (sizeof(Superblock));
	memset(superBlock, 0, sizeof(Superblock));

	memset(diskName, '\0', sizeof(diskName));

	while(!feof(inputFile))
	{
		//lineNum++;
		int readArgs = fscanf(inputFile," %c", &command);

		//precaution to skip empty line in the input file, if any!
		if(EOF == readArgs || 0 == readArgs)
		{
			continue;
		}
		
		lineNum++;

		memset(arg1, '\0', sizeof(arg1));
		memset(line, '\0', sizeof(line));
		memset(num, '\0', sizeof(num));
		arg2 = 0;

		switch(command)
		{
			case 'M':
				if(1 != fscanf(inputFile," %s", arg1))
				{
					fprintf(stderr, "Command Error: %s, %d\n", argv[1], lineNum);
				}
				else
					fs_mount(arg1);
				break;
			case 'C':
				if(fgets(line, sizeof(line), inputFile) == NULL || line[0] == '\n')
					fprintf(stderr,"Command Error: %s, %d\n",argv[1], lineNum);
				else
				{
					int i = 1;
					for(; i < 6; i++)
					{
						if(line[i] == ' ')
							break;
						arg1[i-1] = line[i];
					}

					if(line[i] != ' ')
					{
                        fprintf(stderr, "Command Error: %s, %d\n", argv[1], lineNum);
                        break;
                    }

                    i++;

                    for(int j = 0; j < 3; j++,i++)
                    {
                        if('\n' == line[i])
                            break;
                        num[j] = line[i];
                    }
					
					arg2 = atoi(num);

					if(arg2 > 127)
					{
						fprintf(stderr,"Command Error: %s, %d\n", argv[1], lineNum);
						break;
					}

					fs_create(arg1, arg2);
				}
				break;
			case 'D':
				if (fgets(line, sizeof(line), inputFile) == NULL || line[0] == '\n')
					fprintf(stderr, "Command Error: %s, %d\n", argv[1], lineNum);
				else
				{
					int i = 1;
					for(; i < 6; i++)
					{
						if('\n' == line[i])
							break;
						arg1[i-1] = line[i];
					}
					if('\n' != line[i])
						fprintf(stderr, "Command Error: %s, %d\n", argv[1], lineNum);
					else
						fs_delete(arg1, cwd);
				}
                break;
			case 'R':
				if(2 != fscanf(inputFile," %s %d",arg1, &arg2))
				{
					fprintf(stderr, "Command Error: %s, %d\n", argv[1], lineNum);
				}
				else
					fs_read(arg1,arg2);
				break;
			case 'W':
				if(2 != fscanf(inputFile," %s %d",arg1, &arg2))
				{
					fprintf(stderr, "Command Error: %s, %d\n", argv[1], lineNum);
				}
				else
				{		
					fs_write(arg1, arg2);
				}
				break;
			case 'B':
				if (fgets(line, sizeof(line), inputFile) == NULL || line[0] == '\n') 
				{
					fprintf(stderr, "Command Error: %s, %d\n", argv[1], lineNum);
				}
				else 
				{
					char bufferLine[1024] = {'\0'};

					for(int i = 1; i < 1024; i++)
					{
						if(' ' == line[i])
						{
							fprintf(stderr, "Command Error: %s, %d\n", argv[1], lineNum);
							break;
						}

						if('\n' == line[i])
							break;

						bufferLine[i-1] = line[i];
					}
					
					fs_buff(bufferLine);
				}
				break;
			case 'L':
				if(0 < fscanf(inputFile," %d", &arg2))
				{
					fprintf(stderr, "Command Error: %s, %d\n", argv[1], lineNum);
                }
				else
					fs_ls();
				break;
			case 'E':
				if(fgets(line, sizeof(line), inputFile) == NULL || line[0] == '\n')
					fprintf(stderr, "Command Error: %s, %d\n", argv[1], lineNum);
				else
				{
					{
						int i =	1;
						for(; i < 6; i++)
						{
							if(' ' == line[i])
								break;
							arg1[i-1] = line[i];
						}
						
						if(line[i] != ' ')
						{
							fprintf(stderr, "Command Error: %s, %d\n", argv[1], lineNum);
							break;
						}

						i++;
						
						for(int j = 0; j < 3; j++,i++)
						{
							if('\n' == line[i])
								break;
							num[j] = line[i];
						}


						arg2 = atoi(num);
						fs_resize(arg1, arg2);
					}
				}
				break;
			case 'O':
				fs_defrag();
				break;
			case 'Y':
                if (fgets(line, sizeof(line), inputFile) == NULL || line[0] == '\n') 
                    fprintf(stderr, "Command Error: %s, %d\n", argv[1], lineNum);
				else 
				{
					int i = 1;
					for(; i < 6; i++)
					{
						if(' ' == line[i] || '\n' == line[i])
							break;
						arg1[i-1] = line[i];
					}

					if(line[i] != '\n')
					{
						fprintf(stderr, "Command Error: %s, %d\n", argv[1], lineNum);
						break;
					}
					fs_cd(arg1);
                }
				break;
			default:
				fprintf(stderr, "Command Error: %s, %d\n", argv[1], lineNum);
				fscanf(inputFile, "%*[^\n]");
				break;
			}
		}

		free(superBlock);
		fclose(inputFile);
		return 0;
}
