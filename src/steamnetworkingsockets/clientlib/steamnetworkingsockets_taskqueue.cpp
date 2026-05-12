//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Task queue implementation for SteamNetworkingSockets.
//
#include "steamnetworkingsockets_lowlevel.h"
#include <tier0/memdbgoff.h>

namespace SteamNetworkingSocketsLib {

/////////////////////////////////////////////////////////////////////////////
//
// Task lists
//
/////////////////////////////////////////////////////////////////////////////

ShortDurationLock s_lockTaskQueue( "TaskQueue", LockDebugInfo::k_nOrder_Max ); // Never take another lock while holding this

CTaskTarget::~CTaskTarget()
{
	CancelQueuedTasks();

	// Set to invalid value so we will crash if we have use after free
	m_pFirstTask = (CQueuedTask *)~(uintptr_t)0;
}

void CTaskTarget::CancelQueuedTasks()
{

	// If we have any queued tasks, we need to cancel them
	if ( m_pFirstTask )
	{
		ShortDurationScopeLock scopeLock( s_lockTaskQueue );
		CQueuedTask *pTask = m_pFirstTask;
		while ( pTask )
		{
			CQueuedTask *pNext = pTask->m_pNextTaskForTarget;
			Assert( !pNext || pNext->m_pPrevTaskForTarget == pTask );
			Assert( pTask->m_pTarget == this );
			Assert( pTask->m_eTaskState == CQueuedTask::k_ETaskState_Queued );
			pTask->m_eTaskState = CQueuedTask::k_ETaskState_ReadyToDelete;
			pTask->m_pTarget = nullptr;
			pTask->m_pPrevTaskForTarget = nullptr;
			pTask->m_pNextTaskForTarget = nullptr;
			pTask = pNext;
		}
		m_pFirstTask = nullptr;
	}
}

void CTaskList::QueueTask( CQueuedTask *pTask )
{
	Assert( pTask->m_eTaskState == CQueuedTask::k_ETaskState_Init );
	Assert( !pTask->m_pPrevTaskForTarget );
	Assert( !pTask->m_pNextTaskForTarget );
	Assert( !pTask->m_pNextTaskInQueue );

	ShortDurationScopeLock scopeLock( s_lockTaskQueue );

	// If we have a target, add to list of target's tasks that need to be deleted
	CTaskTarget *pTarget = pTask->m_pTarget;
	if ( pTarget )
	{
		if ( pTarget->m_pFirstTask )
		{
			Assert( pTarget->m_pFirstTask->m_pPrevTaskForTarget == nullptr );
			pTarget->m_pFirstTask->m_pPrevTaskForTarget = pTask;
		}
		pTask->m_pPrevTaskForTarget = nullptr;
		pTask->m_pNextTaskForTarget = pTarget->m_pFirstTask;
		pTarget->m_pFirstTask = pTask;
	}

	if ( m_pLastTask )
	{
		Assert( m_pFirstTask );
		Assert( !m_pLastTask->m_pNextTaskInQueue );
		m_pLastTask->m_pNextTaskInQueue = pTask;
	}
	else
	{
		Assert( !m_pFirstTask );
		m_pFirstTask = pTask;
	}

	m_pLastTask = pTask;

	// Mark task as queued
	pTask->m_eTaskState = CQueuedTask::k_ETaskState_Queued;
}

void CTaskList::RunTasks()
{
	// Quick check for no tasks
	if ( !m_pFirstTask )
		return;

	// Detach the linked list
	s_lockTaskQueue.lock();
	CQueuedTask *pFirstTask = m_pFirstTask;
	m_pFirstTask = nullptr;
	m_pLastTask = nullptr;
	s_lockTaskQueue.unlock();

	// Process items
	CQueuedTask *pTask = pFirstTask;
	while ( pTask )
	{

		// We might have to loop due to lock contention
		for (;;)
		{

			// Already deleted?
			if ( pTask->m_eTaskState != CQueuedTask::k_ETaskState_Queued )
			{
				Assert( pTask->m_eTaskState == CQueuedTask::k_ETaskState_ReadyToDelete );
				Assert( pTask->m_pTarget == nullptr );
				break;
			}

			ShortDurationScopeLock scopeLock;

			// We'll need to lock the queue if they have a target.
			// If their target does not have a locking mechanism, then we
			// assume that it cannot be deleted here.  However, we always need
			// to protect against other tasks getting queued against the target,
			// and we allow that to be done without locking the target.
			if ( pTask->m_pTarget )
				scopeLock.Lock( s_lockTaskQueue );

// FIXME - has not been tested, and also does not have a good mechanism for unlocking.
//			// Do we have a lock we need to take?
//			if ( pTask->m_lockFunc )
//			{
//
//				int msTimeOut = 10;
//				if ( pTask->m_pTarget )
//				{
//					scopeLock.Lock( s_lockTaskQueue );
//
//					// Deleted while we locked?
//					if ( pTask->m_eTaskState != CQueuedTask::k_ETaskState_Queued )
//					{
//						Assert( pTask->m_eTaskState == CQueuedTask::k_ETaskState_ReadyToDelete );
//						Assert( pTask->m_pTarget == nullptr );
//						break;
//					}
//
//					// Use a short timeout and loop, in case we might deadlock
//					msTimeOut = 1;
//				}
//
//				if ( !(*pTask->m_lockFunc)( pTask->m_lockFuncArg, msTimeOut, pTask->m_pszTag ) )
//				{
//					// Other object is busy, or perhaps we are deadlocked?
//					continue;
//				}
//
//				// Deleted while we locked?
//				if ( pTask->m_eTaskState != CQueuedTask::k_ETaskState_Queued )
//				{
//					Assert( pTask->m_eTaskState == CQueuedTask::k_ETaskState_ReadyToDelete );
//					Assert( pTask->m_pTarget == nullptr );
//					break;
//				}
//			}

			// OK, we've got the locks we need and are ready to run.
			// Unlink from the target, if we have one
			CTaskTarget *pTarget = pTask->m_pTarget;
			if ( pTarget )
			{
				CQueuedTask *p = pTask->m_pPrevTaskForTarget;
				CQueuedTask *n = pTask->m_pNextTaskForTarget;
				if ( p )
				{
					Assert( p->m_pTarget == pTarget );
					Assert( p->m_pNextTaskForTarget == pTask );
					p->m_pNextTaskForTarget = n;
					pTask->m_pPrevTaskForTarget = nullptr;
				}
				else
				{
					Assert( pTarget->m_pFirstTask == pTask );
					pTarget->m_pFirstTask = n;
				}
				if ( n )
				{
					Assert( n->m_pPrevTaskForTarget == pTask );
					n->m_pPrevTaskForTarget = p;
					pTask->m_pNextTaskForTarget = nullptr;
				}

				// Note: we must leave the target pointer set
			}

			// We can release this lock now if we took it
			scopeLock.Unlock();

			// !KLUDGE! Deluxe
			if ( this == &g_taskListRunWithGlobalLock )
			{
				// Make sure we hold the lock, and also set the tag for debugging purposes
				SteamNetworkingGlobalLock::AssertHeldByCurrentThread( pTask->m_pszTag );
			}

			// Run the task
			pTask->m_eTaskState = CQueuedTask::k_ETaskState_Running;
			pTask->Run();

			// Mark us as finished
			pTask->m_eTaskState = CQueuedTask::k_ETaskState_ReadyToDelete;
			pTask->m_pTarget = nullptr;
			break;
		}

		// Done, we can delete the task
		CQueuedTask *pNext = pTask->m_pNextTaskInQueue;
		pTask->m_pNextTaskInQueue = nullptr;
		delete pTask;
		pTask = pNext;
	}
}

void CTaskList::DeleteTasks()
{
	ShortDurationScopeLock scopeLock( s_lockTaskQueue );

	CQueuedTask *pNextTask = m_pFirstTask;
	m_pFirstTask = nullptr;
	m_pLastTask = nullptr;

	while ( pNextTask )
	{
		CQueuedTask *pTask = pNextTask;
		pNextTask = pTask->m_pNextTaskInQueue;
		pTask->m_pNextTaskInQueue = nullptr;

		CTaskTarget *pTarget = pTask->m_pTarget;
		if ( pTarget )
		{
			CQueuedTask *pPrevForTarget = pTask->m_pPrevTaskForTarget;
			CQueuedTask *pNextForTarget = pTask->m_pNextTaskForTarget;

			if ( pPrevForTarget )
			{
				Assert( pTarget->m_pFirstTask != nullptr );
				Assert( pTarget->m_pFirstTask != pTask );
				Assert( pPrevForTarget->m_pTarget == pTarget );
				Assert( pPrevForTarget->m_pNextTaskForTarget == pTask );
				pPrevForTarget->m_pNextTaskForTarget = pNextForTarget;
			}
			else
			{
				Assert( pTarget->m_pFirstTask == pTask );
				pTarget->m_pFirstTask = pNextForTarget;
			}

			if ( pNextForTarget )
			{
				Assert( pNextForTarget->m_pTarget == pTarget );
				Assert( pNextForTarget->m_pPrevTaskForTarget == pTask );
				pNextForTarget->m_pPrevTaskForTarget = pPrevForTarget;
			}
		}

		pTask->m_pTarget = nullptr;
		pTask->m_pPrevTaskForTarget = nullptr;
		pTask->m_pNextTaskForTarget = nullptr;
		pTask->m_eTaskState = CQueuedTask::k_ETaskState_ReadyToDelete;

		// Nuke
		delete pTask;
	}
}

CTaskList g_taskListRunWithGlobalLock;
CTaskList g_taskListRunInBackground;

CQueuedTask::~CQueuedTask()
{
	Assert( m_eTaskState == k_ETaskState_Init || m_eTaskState == k_ETaskState_ReadyToDelete );
	Assert( !m_pNextTaskInQueue );
	Assert( !m_pPrevTaskForTarget );
	Assert( !m_pNextTaskForTarget );
}

void CQueuedTask::SetTarget( CTaskTarget *pTarget )
{
	// Can only call this once
	Assert( m_eTaskState == k_ETaskState_Init );
	Assert( m_pTarget == nullptr );
	m_pTarget = pTarget;

	// NOTE: We wait to link task to the target until we actually queue it
}

bool CQueuedTask::RunWithGlobalLockOrQueue( const char *pszTag )
{
	Assert( m_eTaskState == k_ETaskState_Init );
	Assert( m_lockFunc == nullptr );

	// Check if lock is available immediately
	if ( !SteamNetworkingGlobalLock::TryLock( pszTag, 0 ) )
	{
		QueueToRunWithGlobalLock( pszTag );
		return false;
	}

	// Service the queue so we always do items in order
	g_taskListRunWithGlobalLock.RunTasks();

	// Let derived class do work
	m_eTaskState = k_ETaskState_Running;
	Run();

	// Go ahead and unlock now
	SteamNetworkingGlobalLock::Unlock();

	// Self destruct
	m_eTaskState = k_ETaskState_ReadyToDelete;
	m_pTarget = nullptr;
	delete this;

	// We have run
	return true;
}

void CQueuedTask::QueueToRunWithGlobalLock( const char *pszTag )
{
	Assert( m_lockFunc == nullptr );
	if ( pszTag )
		m_pszTag = pszTag;
	g_taskListRunWithGlobalLock.QueueTask( this );

	// Make sure service thread will wake up to do something with this
	WakeServiceThread();

	// NOTE: At this point we are subject to being run or deleted at any time!
}

void CQueuedTask::QueueToRunInBackground()
{
	g_taskListRunInBackground.QueueTask( this );

	//if ( s_bManualPollMode || ( s_pServiceThread && s_pServiceThread->get_id() != std::this_thread::get_id() ) )
	WakeServiceThread();
}

} // namespace SteamNetworkingSocketsLib
