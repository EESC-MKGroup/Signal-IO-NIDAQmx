////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (c) 2016-2019 Leonardo Consoni <leonardojc@protonmail.com>      //
//                                                                            //
//  This file is part of Signal-IO-NIDAQmx.                                   //
//                                                                            //
//  Signal-IO-NIDAQmx is free software: you can redistribute it and/or modify //
//  it under the terms of the GNU Lesser General Public License as published  //
//  by the Free Software Foundation, either version 3 of the License, or      //
//  (at your option) any later version.                                       //
//                                                                            //
//  Signal-IO-NIDAQmx is distributed in the hope that it will be useful,      //
//  but WITHOUT ANY WARRANTY; without even the implied warranty of            //
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the              //
//  GNU Lesser General Public License for more details.                       //
//                                                                            //
//  You should have received a copy of the GNU Lesser General Public License  //
//  along with Signal-IO-NIDAQmx. If not, see <http://www.gnu.org/licenses/>. //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////


#include "signal_io/signal_io.h"

#include "threads/threads.h"
#include "threads/semaphores.h"
#include "threads/khash.h"

//#include "debug/async_debug.h"

#include <NIDAQmx.h>

#define DEBUG_MESSAGE_LENGTH 256

const size_t AQUISITION_BUFFER_LENGTH = 10;
const size_t SIGNAL_INPUT_CHANNEL_MAX_USES = 5;

const bool READ = true;
const bool WRITE = false;

typedef struct _SignalIOTaskData
{
  TaskHandle handle;
  Thread threadID;
  volatile bool isRunning;
  bool mode;
  unsigned int* channelUsesList;
  Semaphore* channelLocksList;
  uInt32 channelsNumber;
  float64* samplesList;
  double* channelValuesList;
}
SignalIOTaskData;

typedef SignalIOTaskData* SignalIOTask;  

KHASH_MAP_INIT_INT( TaskInt, SignalIOTask )
static khash_t( TaskInt )* tasksList = NULL;

DECLARE_MODULE_INTERFACE( SIGNAL_IO_INTERFACE ) 

static void* AsyncReadBuffer( void* );
static void* AsyncWriteBuffer( void* );

static SignalIOTask LoadTaskData( const char* );
static void UnloadTaskData( SignalIOTask );

static bool CheckTask( SignalIOTask );

long int InitDevice( const char* taskName )
{
  if( tasksList == NULL ) tasksList = kh_init( TaskInt );
  
  int taskKey = (int) kh_str_hash_func( taskName );
  
  int insertionStatus;
  khint_t newTaskIndex = kh_put( TaskInt, tasksList, taskKey, &insertionStatus );
  if( insertionStatus > 0 )
  {
    kh_value( tasksList, newTaskIndex ) = LoadTaskData( taskName );
    if( kh_value( tasksList, newTaskIndex ) == NULL )
    {
      //DEBUG_PRINT( "loading task %s failed", taskName );
      EndDevice( taskKey ); 
      return -1;
    }
        
    //DEBUG_PRINT( "new key %d inserted (iterator: %u - total: %u)", kh_key( tasksList, newTaskIndex ), newTaskIndex, kh_size( tasksList ) );
  }
  //else if( insertionStatus == 0 ) { DEBUG_PRINT( "task key %d already exists (iterator %u)", taskKey, newTaskIndex ); }
  
  return (long int) kh_key( tasksList, newTaskIndex );
}

void EndDevice( long int taskID )
{
  khint_t taskIndex = kh_get( TaskInt, tasksList, (khint_t) taskID );
  if( taskIndex == kh_end( tasksList ) ) return;
  
  SignalIOTask task = kh_value( tasksList, taskIndex );
  
  if( CheckTask( task ) ) return;
  
  UnloadTaskData( task );
  
  kh_del( TaskInt, tasksList, (khint_t) taskID );
  
  if( kh_size( tasksList ) == 0 )
  {
    kh_destroy( TaskInt, tasksList );
    tasksList = NULL;
  }
}

void Reset( long int taskID )
{
  return;
}

bool HasError( long int taskID )
{
  return false;
}

size_t GetMaxInputSamplesNumber( long int taskID )
{
  khint_t taskIndex = kh_get( TaskInt, tasksList, (khint_t) taskID );
  if( taskIndex == kh_end( tasksList ) ) return 0;
  
  SignalIOTask task = kh_value( tasksList, taskIndex );
  
  if( task->mode == WRITE ) return 0;
  
  return AQUISITION_BUFFER_LENGTH;
}

size_t Read( long int taskID, unsigned int channel, double* channelSamplesList )
{
  khint_t taskIndex = kh_get( TaskInt, tasksList, (khint_t) taskID );
  if( taskIndex == kh_end( tasksList ) ) return 0;
  
  SignalIOTask task = kh_value( tasksList, taskIndex );
  
  if( channel >= task->channelsNumber ) return 0;
  
  if( !task->isRunning ) return 0;
  
  if( task->mode == WRITE ) return 0;
  
  //Sem_Decrement( task->channelLocksList[ channel ] );
  
  size_t channelAcquiredSamplesCount = (size_t) task->channelValuesList[ channel ];
  memcpy( channelSamplesList, task->samplesList + channel * channelAcquiredSamplesCount, channelAcquiredSamplesCount * sizeof(double) );
  
  return channelAcquiredSamplesCount;
}

bool CheckInputChannel( long int taskID, unsigned int channel )
{
  khint_t taskIndex = kh_get( TaskInt, tasksList, (khint_t) taskID );
  if( taskIndex == kh_end( tasksList ) ) return false;
  
  SignalIOTask task = kh_value( tasksList, taskIndex );
  
  if( task->mode == WRITE ) return false;
  
  if( channel > task->channelsNumber ) return false;
  
  if( task->channelUsesList[ channel ] >= SIGNAL_INPUT_CHANNEL_MAX_USES ) return false;
  
  task->channelUsesList[ channel ]++;
  
  if( !task->isRunning ) task->threadID = Thread_Start( AsyncReadBuffer, task, THREAD_JOINABLE );
  
  return true;
}

bool Write( long int taskID, unsigned int channel, double value )
{
  khint_t taskIndex = kh_get( TaskInt, tasksList, (khint_t) taskID );
  if( taskIndex == kh_end( tasksList ) ) return false;
  
  SignalIOTask task = kh_value( tasksList, taskIndex );
  
  if( !task->isRunning ) return false;
  
  if( task->mode == READ ) return false;
  
  if( channel > task->channelsNumber ) return false;
  
  //Sem_Decrement( task->channelLocksList[ 0 ] );
  
  task->channelValuesList[ channel ] = value;
  
  //Sem_SetCount( task->channelLocksList[ 0 ], 1 );
  
  return true;
}

bool AcquireOutputChannel( long int taskID, unsigned int channel )
{
  khint_t taskIndex = kh_get( TaskInt, tasksList, (khint_t) taskID );
  if( taskIndex == kh_end( tasksList ) ) return false;
  
  //DEBUG_PRINT( "aquiring channel %u from task %d", channel, taskID );
  
  SignalIOTask task = kh_value( tasksList, taskIndex );
  
  if( task->mode == READ ) return false;
  
  if( channel > task->channelsNumber ) return false;
  
  if( task->channelUsesList[ channel ] == 1 ) return false;
  
  task->channelUsesList[ channel ] = 1;
  
  if( !task->isRunning ) task->threadID = Thread_Start( AsyncWriteBuffer, task, THREAD_JOINABLE );
  
  return true;
}

void ReleaseOutputChannel( long int taskID, unsigned int channel )
{
  khint_t taskIndex = kh_get( TaskInt, tasksList, (khint_t) taskID );
  if( taskIndex == kh_end( tasksList ) ) return;
  
  SignalIOTask task = kh_value( tasksList, taskIndex );
  
  if( !task->isRunning ) return;
  
  if( channel > task->channelsNumber ) return;
  
  task->channelUsesList[ channel ] = 0;
  
  (void) CheckTask( task );
}



static void* AsyncReadBuffer( void* callbackData )
{
  SignalIOTask task = (SignalIOTask) callbackData;
  
  int32 aquiredSamplesCount;
  
  task->isRunning = true;
  
  //DEBUG_PRINT( "initializing read thread %lx", THREAD_ID );
  
  while( task->isRunning )
  {
    int errorCode = DAQmxReadAnalogF64( task->handle, AQUISITION_BUFFER_LENGTH, DAQmx_Val_WaitInfinitely, DAQmx_Val_GroupByChannel, 
                                        task->samplesList, task->channelsNumber * AQUISITION_BUFFER_LENGTH, &aquiredSamplesCount, NULL );

    if( errorCode < 0 )
    {
      static char errorMessage[ DEBUG_MESSAGE_LENGTH ];
      DAQmxGetErrorString( errorCode, errorMessage, DEBUG_MESSAGE_LENGTH );
    }
    else
    {
      for( unsigned int channel = 0; channel < task->channelsNumber; channel++ )
      {
        task->channelValuesList[ channel ] = (double) aquiredSamplesCount;
        
        //Sem_SetCount( task->channelLocksList[ channel ], task->channelUsesList[ channel ] );
      }
    }
  }
  
  //DEBUG_PRINT( "ending aquisition thread %x", THREAD_ID );
  
  return NULL;
}

static void* AsyncWriteBuffer( void* callbackData )
{
  SignalIOTask task = (SignalIOTask) callbackData;
  
  int32 writtenSamplesCount;
  
  task->isRunning = true;
  
  while( task->isRunning )
  {
    //Sem_Decrement( task->channelLocksList[ 0 ] );
    
    int errorCode = DAQmxWriteAnalogF64( task->handle, 1, 0, 0.1, DAQmx_Val_GroupByChannel, task->channelValuesList, &writtenSamplesCount, NULL );
    if( errorCode < 0 )
    {
      static char errorMessage[ DEBUG_MESSAGE_LENGTH ];
      DAQmxGetErrorString( errorCode, errorMessage, DEBUG_MESSAGE_LENGTH );
      //DEBUG_PRINT( "error aquiring analog signal: %s", errorMessage );
    }
    
    //Sem_SetCount( task->channelLocksList[ 0 ], 1 );
  }
  
  return NULL;
}

bool CheckTask( SignalIOTask task )
{
  bool isStillUsed = false;
  if( task->channelUsesList != NULL )
  {
    for( size_t channel = 0; channel < task->channelsNumber; channel++ )
    {
      if( task->channelUsesList[ channel ] > 0 )
      {
        isStillUsed = true;
        break;
      }
    }
  }
  
  if( !isStillUsed )
  {
    task->isRunning = false;
    if( task->threadID != THREAD_INVALID_HANDLE ) Thread_WaitExit( task->threadID, 5000 );
  }
  
  return isStillUsed;
}

SignalIOTask LoadTaskData( const char* taskName )
{
  bool loadError = false;
  
  SignalIOTask newTask = (SignalIOTask) malloc( sizeof(SignalIOTaskData) );
  memset( newTask, 0, sizeof(SignalIOTask) );
  
  if( DAQmxLoadTask( taskName, &(newTask->handle) ) >= 0 )
  {
    if( DAQmxGetTaskAttribute( newTask->handle, DAQmx_Task_NumChans, &(newTask->channelsNumber) ) >= 0 )
    {
      //DEBUG_PRINT( "%u signal channels found", newTask->channelsNumber );
  
      newTask->channelUsesList = (unsigned int*) calloc( newTask->channelsNumber, sizeof(unsigned int) );
      memset( newTask->channelUsesList, 0, newTask->channelsNumber * sizeof(unsigned int) );
      
      newTask->samplesList = (float64*) calloc( newTask->channelsNumber * AQUISITION_BUFFER_LENGTH, sizeof(float64) );
      newTask->channelValuesList = (double*) calloc( newTask->channelsNumber, sizeof(double) );
  
      if( DAQmxStartTask( newTask->handle ) >= 0 )
      {
        uInt32 readChannelsNumber;
        DAQmxGetReadAttribute( newTask->handle, DAQmx_Read_NumChans, &readChannelsNumber );
        if( readChannelsNumber > 0 ) 
        {
          newTask->channelLocksList = (Semaphore*) calloc( newTask->channelsNumber, sizeof(Semaphore) );
          for( unsigned int channel = 0; channel < newTask->channelsNumber; channel++ )
            newTask->channelLocksList[ channel ] = Sem_Create( 0, SIGNAL_INPUT_CHANNEL_MAX_USES );
          
          newTask->mode = READ;
        }
        else 
        {
          newTask->channelLocksList = (Semaphore*) calloc( 1, sizeof(Semaphore) );
          newTask->channelLocksList[ 0 ] = Sem_Create( 0, SIGNAL_INPUT_CHANNEL_MAX_USES );
          
          newTask->mode = WRITE;
        }
        
        newTask->isRunning = false;
        newTask->threadID = THREAD_INVALID_HANDLE;
      }
      else
      {
        //DEBUG_PRINT( "error starting task %s", taskName );
        loadError = true;
      }
    }
    else 
    {
      //DEBUG_PRINT( "error getting task %s attribute", taskName );
      loadError = true;
    }
  }
  else 
  {
    //DEBUG_PRINT( "error loading task %s", taskName );
    loadError = true;
  }
  
  if( loadError )
  {
    UnloadTaskData( newTask );
    return NULL;
  }
  
  return newTask;
}

void UnloadTaskData( SignalIOTask task )
{
  if( task == NULL ) return;
  
  //DEBUG_PRINT( "ending task with handle %d", task->handle );

  if( task->mode == READ )
  {
    for( unsigned int channel = 0; channel < task->channelsNumber; channel++ )
      Sem_Discard( task->channelLocksList[ channel ] );
  }
  else
    Sem_Discard( task->channelLocksList[ 0 ] );

  DAQmxStopTask( task->handle );
  DAQmxClearTask( task->handle );

  if( task->channelUsesList != NULL ) free( task->channelUsesList );

  if( task->samplesList != NULL ) free( task->samplesList );
  if( task->channelValuesList != NULL ) free( task->channelValuesList );
  if( task->channelLocksList != NULL ) free ( task->channelLocksList );
  
  free( task );
}
