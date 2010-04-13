#include "elf.h"
#include "eloader.h"
#include "loader.h"
#include "thread.h"
#include "debug.h"
#include "config.h"
#include "menu.h"
#include "graphics.h"

//Comment the following line if you don't want wololo's crappy Fake Ram mechanism
#define FAKEMEM 1

//uncomment the following line if you want to return to the menu when leaving a game (reported to crash)
//#define RETURN_TO_MENU_ON_EXIT

/* eLoader */
/* Entry point: _start() */

#define GET_GP(gp) asm volatile ("move %0, $gp\n" : "=r" (gp))
#define SET_GP(gp) asm volatile ("move $gp, %0\n" :: "r" (gp))

//Any way to have those non globals ?
u32 gp = 0;
u32* entry_point = 0;
u32 hbsize = 4000000; //default value for the hb size roughly 4MB. This value is never used in theory
int g_menu_enabled = 0; // this is set to 1 at runtime if a menu.bin file exists


//Menu variables
u32 * isSet = EBOOT_SET_ADDRESS;
u32 * ebootPath = EBOOT_PATH_ADDRESS;  
u32 * menu_pointer = MENU_LOAD_ADDRESS;
   
   
// Globals for debugging
#ifdef DEBUG
	SceUID dbglog;
	u32 aux = 0;
#endif

// Globals

// NID table for resolving imports
tNIDResolver nid_table[NID_TABLE_SIZE];

// Auxiliary structure to help with syscall estimation
tSceLibrary library_table[MAX_LIBRARIES];

// This function can be used to catch up the import calls when the ELF is up and running
// For debugging only; does not perform the calls
/*
#ifdef DEBUG
		
	void late_import(void)
	{
		asm("\tsw $ra, aux\n");
		int i;
		
		dbglog = sceIoOpen(DEBUG_PATH, PSP_O_CREAT | PSP_O_APPEND | PSP_O_WRONLY, 0777);
		sceIoWrite(dbglog, &aux, sizeof(u32));	
		sceIoClose(dbglog);			
	}
 
#endif
*/

// Gets i-th nid and its associated library
// Returns library name length
int get_lib_nid(int index, char* lib_name, u32* pnid)
{
	int ret;
	unsigned int num_libs, i = 0, count = 0, found = 0;
	tImportedLibrary cur_lib;

	// DEBUG_PRINT("**GETTING NID INDEX:**", &index, sizeof(index));

	index += 1;
	ret = config_num_libraries(&num_libs);
	ret = config_first_library(&cur_lib);

	while (i<num_libs)
	{
		/*
		DEBUG_PRINT("**CURRENT LIB**", cur_lib.library_name, strlen(cur_lib.library_name));
		DEBUG_PRINT(NULL, &(cur_lib.num_imports), sizeof(unsigned int));
		DEBUG_PRINT(NULL, &(cur_lib.nids_offset), sizeof(SceOff));
		*/

		count += cur_lib.num_imports;
		if (index > count)
			ret = config_next_library(&cur_lib);
		else
		{
			ret = config_seek_nid(--index, pnid);
			// DEBUG_PRINT("**SEEK NID**", pnid, sizeof(u32));
			break;
		}
	}

	strcpy(lib_name, cur_lib.library_name);

	return strlen(lib_name);
}

// Autoresolves HBL missing stubs
// Some stubs are compulsory, like sceIo*
void resolve_missing_stubs()
{
	int i, ret;
	unsigned int num_nids;
	u32* cur_stub = *((u32*)ADDR_HBL_STUBS_BLOCK_ADDR);
	u32 nid = 0, syscall = 0;
	char lib_name[MAX_LIBRARY_NAME_LENGTH];

	ret = config_initialize();

	if (ret < 0)
		exit_with_log("**CONFIG INIT FAILED**", &ret, sizeof(ret));

	ret = config_num_nids_total(&num_nids);
	
	for (i=0; i<num_nids; i++)
	{
		if (*cur_stub == 0)
		{

			// DEBUG_PRINT("**CURRENT STUB**", &cur_stub, sizeof(u32));

			ret = get_lib_nid(i, lib_name, &nid);

			// DEBUG_PRINT("**ESTIMATING SYSCALL:**", lib_name, ret);
			// DEBUG_PRINT(" ", &nid, sizeof(nid));
			
			syscall = estimate_syscall(lib_name, nid);

			// DEBUG_PRINT("**RESOLVED SYS**", lib_name, strlen(lib_name));
			// DEBUG_PRINT(" ", &syscall, sizeof(syscall));

			resolve_call(cur_stub, syscall);
		}
		cur_stub += 2;
	}

	sceKernelDcacheWritebackInvalidateAll();

	config_close();
}

// Return index in NID table for the call that corresponds to the NID pointed by "nid"
// Puts call in call_buffer
u32 get_call_nidtable(u32 nid, u32* call_buffer)
{
	int i;
	
	*call_buffer = 0;
	for(i=0; i<NID_TABLE_SIZE; i++) {
            if(nid == nid_table[i].nid)
            {
                if(call_buffer != NULL)
                    *call_buffer = nid_table[i].call;
                break;
            }
        }
	return i;
}

// Returns 1 if stub entry is valid, 0 if it's not
// Just checks if pointers are not NULL
int check_stub_entry(tStubEntry* pentry)
{	
	if( (pentry->library_name) &&
		(pentry->nid_pointer) &&
		(pentry->jump_pointer))
		return 1;
	else
		return 0;
}

// Return real instruction that makes the system call (jump or syscall)
u32 get_good_call(u32* call_pointer)
{
	// Dirty hack here :P but works
	if(*call_pointer & SYSCALL_MASK_IMPORT)
		call_pointer++;
	return *call_pointer;
}

// Fills remaining information on a library
tSceLibrary* complete_library(tSceLibrary* plibrary)
{
	char file_path[MAX_LIBRARY_NAME_LENGTH + 20];
	SceUID nid_file;
	u32 nid;
	unsigned int i;

	// Constructing the file path
	strcpy(file_path, LIB_PATH);
	strcat(file_path, plibrary->library_name);
	strcat(file_path, LIB_EXTENSION);

	// Opening the NID file
	if ((nid_file = sceIoOpen(file_path, PSP_O_RDONLY, 0777)) >= 0)
	{
		// Calculate number of NIDs (size of file/4)
		plibrary->num_library_exports = sceIoLseek(nid_file, 0, PSP_SEEK_END) / sizeof(u32);
		sceIoLseek(nid_file, 0, PSP_SEEK_SET);

		// Search for lowest nid
		i = 0;
		while(sceIoRead(nid_file, &nid, sizeof(u32)) > 0)
		{
			if (plibrary->lowest_nid == nid)
			{
				plibrary->lowest_index = i;
				break;
			}
			i++;
		}

		sceIoClose(nid_file);

		return plibrary;
	}
	else
		return NULL;
}

// Returns index of NID in table
int get_nid_index(u32 nid)
{
	int i;

	for (i=0; i<NID_TABLE_SIZE; i++)
	{
		if (nid_table[i].nid == 0)
			break;
		else if (nid_table[i].nid == nid)
		    return i;
	}

	return -1;
}

// Returns a pointer to the library descriptor
tSceLibrary* get_library_entry(const char* library_name)
{
	unsigned int i;

	if (library_name != NULL)
	{
		for (i=0; i<MAX_LIBRARIES; i++)
		{
			if (library_table[i].library_name == NULL)
				break;
			if (strcmp(library_name, library_table[i].library_name) == 0)
				return &(library_table[i]);
		}
	}

	return NULL;
}

// Checks if a library is in the global library description table
// Returns index if it's there
int get_library_index(char* library_name)
{
	int i;

	if (library_name != NULL)
	{
		for (i=0; i<MAX_LIBRARIES; i++)
		{
			if (library_table[i].library_name == NULL)
				break;
			else if (strcmp(library_name, library_table[i].library_name) == 0)
				return i;
		}
	}

	return -1;
}

/* Fills NID Table */
/* Returns NIDs resolved */
/* "pentry" points to first stub header in game */
int build_nid_table(tNIDResolver *nid_table)
{
	int i = 0, j, k = 0, library_index = -1;
	unsigned int nlib_stubs;
	u32 *cur_nid, *cur_call, syscall_num, good_call, aux1, aux2, nid;
	tSceLibrary *ret;
	tStubEntry *pentry;	
	
	// Zeroing global tables
	memset(nid_table, 0, sizeof(nid_table));
	memset(library_table, 0, sizeof(library_table));
	
	// Getting game's .lib.stub address
	if (aux1 = config_initialize() < 0)
		exit_with_log(" ERROR INITIALIZING CONFIG ", &aux1, sizeof(aux1));

	if (aux1 = config_num_lib_stub(&nlib_stubs) < 0)
	    exit_with_log(" ERROR READING NUMBER OF LIBSTUBS FROM CONFIG ", &aux1, sizeof(aux1));

	if (nlib_stubs == 0)
		exit_with_log(" ERROR: NO LIBSTUBS DEFINED IN CONFIG ", NULL, 0);

	if (aux2 = config_first_lib_stub(&aux1) < 0)
		exit_with_log(" ERROR GETTING FIRST LIBSTUB FROM CONFIG ", &aux2, sizeof(aux2));

	pentry = (tStubEntry*) aux1;

	//DEBUG_PRINT(" build_nid_table() ENTERING MAIN LOOP ", NULL, 0);

	do
	{
		//DEBUG_PRINT(" CURRENT LIBSTUB ", &pentry, sizeof(u32));
		
		// While it's a valid stub header
		while (check_stub_entry(pentry))
		{
			//DEBUG_PRINT(" VALID STUB ", NULL, 0);
			
			// Even if the stub appears to be valid, we shouldn't overflow the static arrays
			if ((i >= NID_TABLE_SIZE) || (k >= MAX_LIBRARIES))
			{
				config_close();
				write_debug(" NID TABLE COUNTER ", &i, sizeof(i));
				write_debug(" LIBRARY TABLE COUNTER ", &k, sizeof(k));
				exit_with_log(" NID/LIBRARY TABLES TOO SMALL ", NULL, 0);
			}			
		
			// Get current NID and resolved syscall/jump pointer
			cur_nid = pentry->nid_pointer;
			cur_call = pentry->jump_pointer;

			// Is this library on the table?
			library_index = get_library_index(pentry->library_name);

			//DEBUG_PRINT(" CURRENT LIBRARY ", NULL, 0);
			//DEBUG_PRINT(pentry->library_name, NULL, 0);

			// New library
			if (library_index < 0)
			{
				//DEBUG_PRINT(" LIBRARY NOT ON TABLE ", NULL, 0);	
				//DEBUG_PRINT(" CURRENT LIBRARY POSITION ", &k, sizeof(k));
				
				strcpy(library_table[k].library_name, pentry->library_name);
				
				good_call = get_good_call(cur_call);
				
				// Initialize lowest syscall on library table				
				library_table[k].lowest_syscall = GET_SYSCALL_NUMBER(good_call);
				library_table[k].lowest_nid = *cur_nid;

				// Get number of syscalls imported
				library_table[k].num_known_exports = pentry->stub_size;			

				// DEBUG_PRINT(" NEW LIBRARY ", (void*)library_table[k].library_name, strlen(library_table[k].library_name));

				// JUMP call
				if (good_call & SYSCALL_MASK_RESOLVE)
				{
					library_table[k].calling_mode = JUMP_MODE;
			
					// Browse all stubs defined by this header and fill NID table
					for(j=0; j<pentry->stub_size; j++)
					{
						nid = *cur_nid;
						
						// Only insert NID if it's a new one
						if (get_nid_index(nid) < 0)
						{
							nid_table[i].nid = nid;			
							nid_table[i].call = get_good_call(cur_call);						
							//DEBUG_PRINT(" NID INSERTED ", &nid_table[i].nid, sizeof(u32));
							//DEBUG_PRINT(" CURRENT POSITION ", &i, sizeof(i));
							i++;							
						}
						cur_nid++;
						cur_call += 2;
					}
				}

				// SYSCALL call
				else
				{					
					library_table[k].calling_mode = SYSCALL_MODE;
			
					// Browse all stubs defined by this header
					for(j=0; j<pentry->stub_size; j++)
					{
						nid = *cur_nid;
						
						// If NID is already in, don't put it again 
						if (get_nid_index(nid) < 0)
						{
							// Fill NID table
							nid_table[i].nid = nid;			
							nid_table[i].call = get_good_call(cur_call);

							// Check lowest syscall
							syscall_num = GET_SYSCALL_NUMBER(nid_table[i].call);
							if (syscall_num < library_table[k].lowest_syscall)
							{
								library_table[k].lowest_syscall = syscall_num;
								library_table[k].lowest_nid = nid_table[i].nid;
							}
							//DEBUG_PRINT(" NID INSERTED ", &nid_table[i].nid, sizeof(u32));
							//DEBUG_PRINT(" CURRENT POSITION ", &i, sizeof(i));
							i++;
						}
						cur_nid++;
						cur_call += 2;
					}

					// Fill remaining data
					ret = complete_library(&(library_table[k]));
				}			

				// Next library entry
				k++;
			}

			// Old library
			else
			{
				//DEBUG_PRINT(" LIBRARY ON TABLE ", NULL, 0);	
				//DEBUG_PRINT(" LIBRARY POSITION ", &library_index, sizeof(library_index));

				good_call = get_good_call(cur_call);

				// JUMP
				if (good_call & SYSCALL_MASK_RESOLVE)
				{
					if (library_table[library_index].calling_mode != JUMP_MODE)
					{
						config_close();
						exit_with_log(" ERROR OLD CALL MODE IS JUMP, NEW IS SYSCALL ", &library_index, sizeof(library_index));
					}
					
					// Browse all stubs defined by this header and fill NID table
					for(j=0; j<pentry->stub_size; j++)
					{
						nid = *cur_nid;
						
						// Only insert NID if it's a new one
						if (get_nid_index(nid) < 0)
						{
							nid_table[i].nid = nid;			
							nid_table[i].call = get_good_call(cur_call);
							//DEBUG_PRINT(" NID INSERTED ", &nid_table[i].nid, sizeof(u32));
							//DEBUG_PRINT(" CURRENT POSITION ", &i, sizeof(i));
							i++;
							library_table[library_index].num_known_exports++;
						}
						cur_nid++;
						cur_call += 2;
					}
				}

				// SYSCALL
				else
				{
					if (library_table[library_index].calling_mode != SYSCALL_MODE)
					{
						config_close();
						exit_with_log(" ERROR OLD CALL MODE IS SYSCALL, NEW IS JUMP ", &library_index, sizeof(library_index));
					}
					
					// Browse all stubs defined by this header
					for(j=0; j<pentry->stub_size; j++)
					{
						nid = *cur_nid;
						
						// If NID is already in, don't put it again 
						if (get_nid_index(nid) < 0)
						{
							// Fill NID table
							nid_table[i].nid = nid;			
							nid_table[i].call = get_good_call(cur_call);
						
							// Check lowest syscall
							syscall_num = GET_SYSCALL_NUMBER(nid_table[i].call);
							if (syscall_num < library_table[library_index].lowest_syscall)
							{
								library_table[library_index].lowest_syscall = syscall_num;
								library_table[library_index].lowest_nid = nid_table[i].nid;
							}
							//DEBUG_PRINT(" NID INSERTED ", &nid_table[i].nid, sizeof(u32));
							//DEBUG_PRINT(" CURRENT POSITION ", &i, sizeof(i));
							i++;
							library_table[library_index].num_known_exports++;
						}
						cur_nid++;
						cur_call += 2;
					}
				}
			}

			// Next entry
			pentry++;
		}
		
		nlib_stubs--;

		// Next .lib.stub
		if (nlib_stubs > 0)
		{
			aux2 = config_next_lib_stub(&aux1);
			if (aux2 < 0)
				exit_with_log(" ERROR GETTING NEXT LIBSTUB FROM CONFIG ", &aux2, sizeof(aux2));
			pentry = (tStubEntry*) aux1;
		}
		else
			break;

		sceKernelDelayThread(100000);
		
	} while(1);

	#ifdef DEBUG
		int c = 0;
		u32 syscall;
		write_debug(" NID TABLE (NID/SYSCALL NUMBER) DUMP ", NULL, 0);
		while (c <= i)
		{
			syscall = GET_SYSCALL_NUMBER(nid_table[c].call);
			write_debug(" ", &nid_table[c++].nid, sizeof(u32));
			write_debug(" ", &syscall, sizeof(syscall));		
		}
		write_debug(" LIBRARY TABLE DUMP ", NULL, 0);
		c = 0;
		while (c <= k)
			write_debug(" ", &library_table[c++], sizeof(tSceLibrary));
	#endif

	config_close();

	DEBUG_PRINT(" build_nid_table() EXITING ", NULL, 0);

	return i;
}

// Subsitutes the right instruction
void resolve_call(u32 *call_to_resolve, u32 call_resolved)
{
	/*
	#ifdef DEBUG
		dbglog = sceIoOpen(DEBUG_PATH, PSP_O_CREAT|PSP_O_WRONLY|PSP_O_APPEND, 0777);
		if(dbglog >= 0)
		{
			sceIoWrite(dbglog, " CALL TO RESOLVE ", strlen(" CALL TO RESOLVE "));
			sceIoWrite(dbglog, &call_to_resolve, sizeof(u32*));
			sceIoWrite(dbglog, " CALL RESOLVED ", strlen(" CALL RESOLVED "));
			sceIoWrite(dbglog, &call_resolved, sizeof(u32));
			sceIoClose(dbglog);
		}
	#endif
	*/

	// SYSCALL
	if(!(call_resolved & SYSCALL_MASK_RESOLVE))
	{
		/*
		#ifdef DEBUG
			dbglog = sceIoOpen(DEBUG_PATH, PSP_O_CREAT|PSP_O_WRONLY|PSP_O_APPEND, 0777);
			if(dbglog >= 0)
			{
				sceIoWrite(dbglog, " SYSCALL ", strlen(" SYSCALL "));
				sceIoClose(dbglog);
			}
		#endif
		*/
		*call_to_resolve = JR_RA_OPCODE;
		*(++call_to_resolve) = call_resolved;
	}
	
	// JUMP
	else
	{
		/*
		#ifdef DEBUG
			dbglog = sceIoOpen(DEBUG_PATH, PSP_O_CREAT|PSP_O_WRONLY|PSP_O_APPEND, 0777);
			if(dbglog >= 0)
			{
				sceIoWrite(dbglog, " JUMP ", strlen(" JUMP "));
				sceIoClose(dbglog);
			}
		#endif
		*/
		*call_to_resolve = call_resolved;
		*(++call_to_resolve) = NOP_OPCODE;
	}

	// This is a jump to "late_import" function
	// To use late importing (only for debugging for now), uncomment this and the function late_import(), 
	// and comment out previous code
	/*
	#ifdef DEBUG		
		*call_to_resolve = MAKE_CALL(late_import);
	#endif	
	*/
}


void main_loop() 
{
    isSet[0] = 0;
	
    loadMenu();
	
    while(! isSet[0])
        sceKernelDelayThread(5000);
	
    start_eloader((char *)ebootPath, 1);
}

// Jumps to ELF's entry point
void runThread(SceSize args, void *argp)
{
	void (*start_entry)(SceSize, void*) = entry_point;
	sceKernelFreePartitionMemory(*((SceUID*)ADDR_HBL_BLOCK_UID));
	sceKernelFreePartitionMemory(*((SceUID*)ADDR_HBL_STUBS_BLOCK_UID));	
	start_entry(args, argp);
}

/* Hooks for some functions used by Homebrews */
SceUID _hook_sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int currentPriority,
                             	   int stackSize, SceUInt attr, SceKernelThreadOptParam *option)
{ 
    u32 gp_bak = 0;
	SceUID res;
	
    if (gp) 
	{
        GET_GP(gp_bak);
        SET_GP(gp);
    }
	
    entry_point = entry;
    res = sceKernelCreateThread(name, runThread,  currentPriority,stackSize, attr, option);
	
    if (gp) 
	{
        SET_GP(gp_bak);
    }
	
    return res;
}

#ifdef RETURN_TO_MENU_ON_EXIT
void  _hook_sceKernelExitGame() 
{
	SceUID thid = sceKernelCreateThread("HBL", main_loop, 0x18, 0x10000, 0, NULL);
	
	if (thid >= 0)
	{
		thid = sceKernelStartThread(thid, 0, NULL);
    }
    sceKernelExitDeleteThread(0);
}
#endif

#ifdef FAKEMEM
int  _hook_sceKernelMaxFreeMemSize () 
{
 return 0x09EC8000 - PRX_LOAD_ADDRESS - hbsize;
}

void *  _hook_sceKernelGetBlockHeadAddr (SceUID mid) 
{
    if (mid == 0x05B8923F)
        return PRX_LOAD_ADDRESS + hbsize;
    return 0;
}

SceUID _hook_sceKernelAllocPartitionMemory(SceUID partitionid, const char *name, int type, SceSize size, void *addr)
{
	SceUID ret = 0x05B8923F;

	return ret;
}
#endif

/* WIP
// A function that just returns "ok" but does nothing
int _hook_ok()
{
    return 1;
}
*/

/* Resolves imports in ELF's program section already loaded in memory */
/* Uses game's imports to do the resolving (this can be further improved) */
/* Returns number of resolves */
unsigned int resolve_imports(tStubEntry* pstub_entry, unsigned int stubs_size)
{
	unsigned int i,j,nid_index;
	u32* cur_nid;
	u32* cur_call;
	u32 real_call;
	unsigned int resolving_count = 0;

	DEBUG_PRINT("RESOLVING IMPORTS", &stubs_size, sizeof(unsigned int));

	/* Browse ELF stub headers */
	for(i=0; i<stubs_size; i+=sizeof(tStubEntry))
	{
		//DEBUG_PRINT("POINTER TO STUB ENTRY:", &pstub_entry, sizeof(u32*));

		cur_nid = pstub_entry->nid_pointer;
		cur_call = pstub_entry->jump_pointer;

		/* For each stub header, browse all stubs */
		for(j=0; j<pstub_entry->stub_size; j++)
		{

			//DEBUG_PRINT("Current nid:", cur_nid, sizeof(u32*));
			//DEBUG_PRINT("Current call:", &cur_call, sizeof(u32*));

			/* Get syscall/jump instruction for current NID */
			nid_index = get_call_nidtable(*cur_nid, &real_call);

			//DEBUG_PRINT(" REAL CALL (TABLE) ", &real_call, sizeof(u32));
            
            switch (*cur_nid) 
			{
                case 0x446D8DE6: //sceKernelCreateThread
                    real_call = MAKE_JUMP(_hook_sceKernelCreateThread);
                    break;

#ifdef RETURN_TO_MENU_ON_EXIT                
                case 0x05572A5F: //sceKernelExitGame
                    if (g_menu_enabled)
                        real_call = MAKE_JUMP(_hook_sceKernelExitGame);
                    break;
#endif

#ifdef FAKEMEM    
                case 0xA291F107:
                    DEBUG_PRINT("mem trick", NULL, 0);
                    real_call = MAKE_JUMP(_hook_sceKernelMaxFreeMemSize);
                    break;
                case 0x9D9A5BA1:
                    DEBUG_PRINT("mem trick", NULL, 0);
                    real_call = MAKE_JUMP(_hook_sceKernelGetBlockHeadAddr);
                    break;
                case 0x237DBD4F:
                    DEBUG_PRINT("mem trick", NULL, 0);
                    real_call = MAKE_JUMP(_hook_sceKernelAllocPartitionMemory);
                    break; 
#endif
					
/*
Work in progress, attempt for the mp3 library not to fail                  
                case 0x07EC321A:	//sceMp3ReserveMp3Handle
                case 0x0DB149F4:	//sceMp3NotifyAddStreamData
                case 0x2A368661:	//sceMp3ResetPlayPosition
                case 0x354D27EA:	//	sceMp3GetSumDecodedSample
                case 0x35750070:	//	sceMp3InitResource
                case 0x3C2FA058:	//	sceMp3TermResource
                case 0x3CEF484F:	//	sceMp3SetLoopNum
                case 0x44E07129:	//	sceMp3Init
                case 0x732B042A:	//	sceMp3EndEntry
                case 0x7F696782:	//	sceMp3GetMp3ChannelNum
                case 0x87677E40:	//	sceMp3GetBitRate
                case 0x87C263D1:	//	sceMp3GetMaxOutputSample
                case 0x8AB81558:	//	sceMp3StartEntry
                case 0x8F450998:	//	sceMp3GetSamplingRate
                case 0xA703FE0F:	//	sceMp3GetInfoToAddStreamData
                case 0xD021C0FB:	//	sceMp3Decode
                case 0xD0A56296:	//	sceMp3CheckStreamDataNeeded
                case 0xD8F54A51:	//	sceMp3GetLoopNum
                case 0xF5478233:	//	sceMp3ReleaseMp3Handle
                    real_call = MAKE_JUMP(_hook_genericSuccess);
                    break;
*/
                   
            }
            
			/* If NID not found in MoHH imports */
			/* Syscall estimation if library available */
			if (real_call == 0)
			{
				/*
				#ifdef DEBUG
					dbglog = sceIoOpen(DEBUG_PATH, PSP_O_CREAT|PSP_O_WRONLY|PSP_O_APPEND, 0777);
					if (dbglog >= 0)
					{
						sceIoWrite(dbglog, " LIBRARY NAME ", strlen(" LIBRARY NAME "));
						sceIoWrite(dbglog, pstub_entry->library_name, strlen(pstub_entry->library_name));					
						sceIoClose(dbglog);
					}
				#endif
				*/

				real_call = estimate_syscall(pstub_entry->library_name, *cur_nid);
				
				/* Commit changes to RAM */
				sceKernelDcacheWritebackInvalidateAll();

				/*
				#ifdef DEBUG	
					dbglog = sceIoOpen(DEBUG_PATH, PSP_O_CREAT|PSP_O_WRONLY|PSP_O_APPEND, 0777);
					if (dbglog >= 0)
					{
						sceIoWrite(dbglog, " ESTIMATED SYSCALL VALUE ", strlen(" ESTIMATED SYSCALL VALUE "));
						sceIoWrite(dbglog, &real_call, sizeof(real_call));
						sceIoClose(dbglog);
					}
				#endif
				*/

			}

			/* If it's an instruction, resolve it */
			/* 0xC -> syscall 0 */
			/* Jumps are always > 0xC */		
			if(real_call > 0xC)
			{	
				/* Write it in ELF stubs memory */
				resolve_call(cur_call, real_call);
				resolving_count++;
			}

			sceKernelDcacheWritebackInvalidateAll();

			/*
			#ifdef DEBUG
				sceIoWrite(dbglog, &real_call, sizeof(u32));	
				sceIoWrite(dbglog, cur_call, sizeof(u32)*2);
			#endif
			*/

			cur_nid++;
			cur_call += 2;
		}
		
		pstub_entry++;
	}
	
	return resolving_count;	
}

// Relocates based on a MIPS relocation type
// Returns 0 on success, -1 on fail
int relocate_entry(tRelEntry reloc_entry)
{
	u32 buffer = 0, code = 0, offset = 0, offset_target, i;
	
	/*
	#ifdef DEBUG
		dbglog = sceIoOpen(DEBUG_PATH, PSP_O_CREAT | PSP_O_WRONLY | PSP_O_APPEND, 0777);
		sceIoWrite(dbglog, " RELOCATING ", strlen(" RELOCATING "));
		sceIoWrite(dbglog, &reloc_entry, sizeof(tRelEntry));
		sceIoClose(dbglog);
	#endif
	*/

	// Actual offset
	offset_target = (u32)reloc_entry.r_offset + (u32)PRX_LOAD_ADDRESS;
	
	/*
	#ifdef DEBUG
		dbglog = sceIoOpen(DEBUG_PATH, PSP_O_CREAT | PSP_O_WRONLY | PSP_O_APPEND, 0777);
		sceIoWrite(dbglog, &(offset_target), sizeof(u32));
		sceIoClose(dbglog);
	#endif
	*/

	// Load word to be relocated into buffer
    u32 misalign = offset_target%4;
    if (misalign) {
        u32 array[2];
        array[0] = *(u32*)(offset_target - misalign);
        array[1] = *(u32*)(offset_target + 4 - misalign);
        u8* array8 = (u8*)&array;
        u8* buffer8 = (u8*)&buffer;
        for (i = 3 + misalign; i >= misalign; --i){
            buffer8[i-misalign] = array8[i];
        }
    } else {
        buffer = *(u32*)offset_target;
    }

	/*	
	#ifdef DEBUG
		dbglog = sceIoOpen(DEBUG_PATH, PSP_O_CREAT | PSP_O_WRONLY | PSP_O_APPEND, 0777);
		sceIoWrite(dbglog, &buffer, sizeof(u32));
		sceIoClose(dbglog);
	#endif
	*/
	
	//Relocate depending on reloc type
	switch(ELF32_R_TYPE(reloc_entry.r_info))
	{
		// Nothing
		case R_MIPS_NONE:
			return 0;
			
		// 32-bit address
		case R_MIPS_32:
			buffer += PRX_LOAD_ADDRESS;
			break;
		
		// Jump instruction
		case R_MIPS_26:    
			code = buffer & 0xfc000000;
			offset = (buffer & 0x03ffffff) << 2;
			offset += PRX_LOAD_ADDRESS;
			buffer = ((offset >> 2) & 0x03ffffff) | code;
			break;
		
		// Low 16 bits relocate as high 16 bits
		case R_MIPS_HI16:     
			offset = (buffer << 16) + PRX_LOAD_ADDRESS;
			offset = offset >> 16;
			buffer = (buffer & 0xffff0000) | offset;
			break;
		
		// Low 16 bits relocate as low 16 bits
		case R_MIPS_LO16:       
			offset = (buffer & 0x0000ffff) + PRX_LOAD_ADDRESS;
			buffer = (buffer & 0xffff0000) | (offset & 0x0000ffff);
			break;
		
		default:
			return -1;		
	}
	
	// Restore relocated word
    if (misalign) {
        u32 array[2];
        array[0] = *(u32*)(offset_target - misalign);
        array[1] = *(u32*)(offset_target + 4 - misalign);
        u8* array8 = (u8*)&array;
        u8* buffer8 = (u8*)&buffer;
        for (i = 3 + misalign; i >= misalign; --i){
             array8[i] = buffer8[i-misalign];
        }
        *(u32*)(offset_target - misalign) = array[0];
        *(u32*)(offset_target + 4 - misalign) = array[1];
    } else { 
        *(u32*)offset_target = buffer;
    }
	
	/*
	#ifdef DEBUG
		dbglog = sceIoOpen(DEBUG_PATH, PSP_O_CREAT | PSP_O_WRONLY | PSP_O_APPEND, 0777);
		sceIoWrite(dbglog, offset_target, sizeof(u32));
		sceIoClose(dbglog);
	#endif
	*/

	return 0;
}

// Relocates PRX sections that need to
// Returns number of relocated entries
unsigned int relocate_sections(SceUID elf_file, u32 start_offset, Elf32_Ehdr *pelf_header)
{
	Elf32_Half i;
	Elf32_Shdr sec_header;
	Elf32_Off strtab_offset;
	Elf32_Off cur_offset;	
	char section_name[40];
	unsigned int j, section_name_size, entries_relocated = 0, num_entries;
	tRelEntry* reloc_entry;
	
	// Seek string table
	sceIoLseek(elf_file, start_offset + pelf_header->e_shoff + pelf_header->e_shstrndx * sizeof(Elf32_Shdr), PSP_SEEK_SET);
	sceIoRead(elf_file, &sec_header, sizeof(Elf32_Shdr));
	strtab_offset = sec_header.sh_offset;

	// First section header
	cur_offset = pelf_header->e_shoff;
	
	DEBUG_PRINT(" NUMBER OF SECTIONS ", &(pelf_header->e_shnum), sizeof(Elf32_Half));

	// Browse all section headers
	for(i=0; i<pelf_header->e_shnum; i++)
	{
		// Get section header
		sceIoLseek(elf_file, start_offset + cur_offset, PSP_SEEK_SET);
		sceIoRead(elf_file, &sec_header, sizeof(Elf32_Shdr));
		
		/*
		// Get section name
		section_name_size = elf_read_string(elf_file, strtab_offset + sec_header.sh_name, section_name);

		#ifdef DEBUG
			dbglog = sceIoOpen(DEBUG_PATH, PSP_O_CREAT | PSP_O_WRONLY | PSP_O_APPEND, 0777);
			sceIoWrite(dbglog, " SECTION NAME ", strlen(" SECTION NAME "));
			sceIoWrite(dbglog, section_name, strlen(section_name));
			sceIoClose(dbglog);
		#endif
		*/

		if(sec_header.sh_type == LOPROC)
		{
			// DEBUG_PRINT(" RELOCATING SECTION ", NULL, 0);
			
			// Allocate memory for section
			num_entries = sec_header.sh_size / sizeof(tRelEntry);
			reloc_entry = malloc(sec_header.sh_size);
			if(!reloc_entry)
			{
				DEBUG_PRINT(" CANNOT ALLOCATE MEMORY FOR SECTION ", NULL, 0);
				continue;
			}
			
			// Read section
			sceIoLseek(elf_file, start_offset + sec_header.sh_offset, PSP_SEEK_SET);
			sceIoRead(elf_file, reloc_entry, sec_header.sh_size);

			for(j=0; j<num_entries; j++)
			{
				//DEBUG_PRINT(" RELOC_ENTRY ", &(reloc_entry[j]), sizeof(tRelEntry));	
                relocate_entry(reloc_entry[j]);
				entries_relocated++;
			}
			
			// Free section memory
			free(reloc_entry);
		}

		// Next section header
		cur_offset += sizeof(Elf32_Shdr);
	}
	
	// DEBUG_PRINT(" FINISHED RELOCATING  ",NULL, 0);

	// All relocation section processed
	return entries_relocated;
}


// Jumps to ELF's entry point
void execute_elf(SceSize args, void *argp)
{
	void (*start_elf)(SceSize, void*) = entry_point;	
	start_elf(args, argp);
}

// Allocates memory for homebrew so it doesn't overwrite itself
void allocate_memory(u32 size, void* addr)
{
#ifndef FAKEMEM
	SceUID mem;
	
	DEBUG_PRINT(" ALLOC EXECUTABLE MEMORY ", NULL, 0);
	mem = sceKernelAllocPartitionMemory(2, "ELFMemory", PSP_SMEM_Addr, size, addr);
	if(mem < 0)
		DEBUG_PRINT(" sceKernelAllocPartitionMemory FAILED ", &mem, sizeof(mem));
#endif
}

// HBL entry point
// Needs path to ELF or EBOOT
void start_eloader(char *eboot_path, int is_eboot)
{
	unsigned int num_nids, stubs_size, stubs_resolved;
	unsigned int sections_relocated;
	tStubEntry* pstub_entry;
	SceUID elf_file, thid;
	Elf32_Ehdr elf_header;
	u32 offset = 0;

	//DEBUG_PRINT("EBOOT:", eboot_path, strlen(eboot_path));

	// Extracts ELF from PBP
	if (is_eboot)		
		elf_file = elf_eboot_extract_open(eboot_path, &offset);
	// Plain ELF
	else
		elf_file = sceIoOpen(eboot_path, PSP_O_RDONLY, 0777);
	
	// Read ELF header
	sceIoRead(elf_file, &elf_header, sizeof(Elf32_Ehdr));
	
	DEBUG_PRINT(" ELF TYPE ", &(elf_header.e_type), sizeof(Elf32_Half));
    
    gp = getGP(elf_file, offset, &elf_header); 
    //DEBUG_PRINT(" GP IS: ", &gp, sizeof(u32));           

	// Static ELF
	if(elf_header.e_type == (Elf32_Half) ELF_STATIC)
	{	
		DEBUG_PRINT(" STATIC ELF ", NULL, 0);		

		// Load ELF program section into memory
		hbsize = elf_load_program(elf_file, offset, &elf_header, allocate_memory);		
	
		// Locate ELF's .lib.stubs section */
		stubs_size = elf_find_imports(elf_file, offset, &elf_header, &pstub_entry);
	}
	
	// Relocatable ELF (PRX)
	else if(elf_header.e_type == (Elf32_Half) ELF_RELOC)
	{	
		DEBUG_PRINT(" PRX ELF ", NULL, 0);
   
		// Load program section into memory and also get stub headers
		stubs_size = prx_load_program(elf_file, offset, &elf_header, &pstub_entry, &hbsize, allocate_memory);
		
		/*
		#ifdef DEBUG
			dbglog = sceIoOpen(DEBUG_PATH, PSP_O_CREAT | PSP_O_WRONLY | PSP_O_APPEND, 0777);
			sceIoWrite(dbglog, " STUBS SIZE ", strlen(" STUBS SIZE "));
			sceIoWrite(dbglog, &stubs_size, sizeof(int));
			sceIoClose(dbglog);
		#endif
		*/

		//Relocate all sections that need to
		sections_relocated = relocate_sections(elf_file, offset, &elf_header);
	}
	
	// Unknown ELF type
	else
	{
		exit_with_log(" UNKNOWN ELF TYPE ", NULL, 0);
	}
	
	// Resolve ELF's stubs with game's stubs and syscall estimation */
	stubs_resolved = resolve_imports(pstub_entry, stubs_size);

   
	// No need for ELF file anymore
	sceIoClose(elf_file);	

	// Go ELF, go!
	if (elf_header.e_type == (Elf32_Half) ELF_RELOC) {
		elf_header.e_entry = (u32)elf_header.e_entry + (u32)PRX_LOAD_ADDRESS;
        gp += (u32)PRX_LOAD_ADDRESS;
    }	
	
	// DEBUG_PRINT(" PROGRAM SECTION START ", &(elf_header.e_entry), sizeof(Elf32_Addr));		
	DEBUG_PRINT(" EXECUTING ELF ", NULL, 0);    
    
    // Commit changes to RAM
	sceKernelDcacheWritebackInvalidateAll();

	// Create and start eloader thread
    if (gp)
        SET_GP(gp);
	entry_point = (u32 *)elf_header.e_entry;
	thid = sceKernelCreateThread("homebrew", runThread, 0x18, 0x10000, 0, NULL);

	if(thid >= 0)
	{
		thid = sceKernelStartThread(thid, strlen(eboot_path) + 1, (void *)eboot_path);
    } 
	else 
	{
        exit_with_log(" HB Launch failed ", NULL, 0);
    }
}

/* Loads a Basic menu as a thread
* In the future, we might want the menu to be an actual homebrew
*/
void loadMenu()
{
    // Just trying the basic functions used by the menu
    SceUID id = -1;
    int attempts = 0;
	SceUID menu_file;
    SceOff file_size;
	int bytes_read;
	SceIoDirent entry;
	SceUID menuThread;
		
    DebugPrint("Loading Menu");
	
    while ((id < 0) && (attempts < MAX_REESTIMATE_ATTEMPTS))
	{
        attempts++;
        id = sceIoDopen("ms0:");
        if (id <= 0)
		{
            DEBUG_PRINT(" sceIoDopen syscall estimation failed, attempt to reestimate ",NULL, 0);
            reestimate_syscall(0xB29DDF9C, attempts); //sceIoDopen TODO move to config ?
        }
    }
	
    if (id < 0) 
	{
        DebugPrint("Loading Menu Failed (syscall ?)");
        exit_with_log(" FATAL, sceIoDopen syscall estimation failed ",NULL, 0);
    }
	
	else 
	{
        attempts = 0;        
        memset(&entry, 0, sizeof(SceIoDirent)); 
        while (sceIoDread(id, &entry) <= 0 && attempts < 10) 
		{
            attempts++;
            DEBUG_PRINT(" sceIoDread syscall estimation failed, attempt to reestimate ",NULL, 0);
            reestimate_syscall(0xE3EB004C, attempts); //sceIoDread TODO move to config ?
            memset(&entry, 0, sizeof(SceIoDirent));
        }
    }
	
    sceIoDclose(id);

	//DEBUG_PRINT(" LOADER RUNNING ", NULL, 0);	

	if ((menu_file = sceIoOpen(MENU_PATH, PSP_O_RDONLY, 0777)) < 0)
		exit_with_log(" FAILED TO LOAD MENU ", &menu_file, sizeof(menu_file));

	// Get MENU size
	file_size = sceIoLseek(menu_file, 0, PSP_SEEK_END);
	sceIoLseek(menu_file, 0, PSP_SEEK_SET);    
    
	// Load MENU to buffer
	if ((bytes_read = sceIoRead(menu_file, (void*)menu_pointer, file_size)) < 0)
		exit_with_log(" ERROR READING MENU ", &bytes_read, sizeof(bytes_read));
        
    void (*start_entry)(SceSize, void*) = menu_pointer;	 
	menuThread = sceKernelCreateThread("menu", start_entry, 0x18, 0x10000, 0, NULL);

	if(menuThread >= 0)
	{
		menuThread = sceKernelStartThread(menuThread, 0, NULL);
    } 

	else 
	{
        exit_with_log(" Menu Launch failed ", NULL, 0);
    }        
}

// Returns 1 if a given file exists, 0 otherwise
int file_exists(const char * filename)
{
    SceUID id = sceIoOpen(filename, PSP_O_RDONLY, 0777);
    if (id < 0) 
		return 0;
    sceIoClose(id);
    return 1;
}

// HBL main thread
int start_thread(SceSize args, void *argp)
{
	int num_nids;
	
	// Build NID table
    DebugPrint("Build Nids table");
	num_nids = build_nid_table(nid_table);
    DEBUG_PRINT(" NUM NIDS ", &num_nids, sizeof(int));
	
	if(num_nids > 0)
	{	
		// FIRST THING TO DO!!!
        DebugPrint("Resolving Missing Stubs");
		resolve_missing_stubs();
	
		// Free memory
        DebugPrint("Free memory");
		free_game_memory();
        DEBUG_PRINT(" START HBL ", NULL, 0);

        // Start the menu or run directly the hardcoded eboot      
        if (file_exists(EBOOT_PATH))
            start_eloader(EBOOT_PATH, 1);
        else if (file_exists(ELF_PATH))
            start_eloader(ELF_PATH, 0);
        else 
		{
            g_menu_enabled = 1;
            main_loop();
        }
	}
	
	// Exit thread
    DebugPrint("Exiting HBL Thread");
	sceKernelExitDeleteThread(0);
	
	return 0;
}

// Entry point
void _start(unsigned long, unsigned long *) __attribute__ ((section (".text.start")));
void _start(unsigned long arglen, unsigned long *argp)
{	
	SceUID thid;    
    void *fb = (void *)0x444000000;
	
    sceDisplaySetFrameBuf(fb, 512, PSP_DISPLAY_PIXEL_FORMAT_8888, 1);
    SetColor(0);
    DebugPrint("Starting HBL -- http://code.google.com/p/valentine-hbl");
	
	// Create and start eloader thread
	thid = sceKernelCreateThread("HBL", start_thread, 0x18, 0x10000, 0, NULL);
	
	if(thid >= 0)
	{
		thid = sceKernelStartThread(thid, 0, NULL);
	}
	
	sceKernelExitDeleteThread(0);

	// Never executed (hopefully)
	return 0;
}

// Big thanks to people who share information !!!