// StaleFrame.h -- the verdict of frame generation's stale real-frame guard (pure, unit-tested).
//
// Before a new real frame (a commit the host has not seen) is copied into the ring, a GPU probe
// (cs_fg_stale_probe.comp, FrameGenHost.cpp's StaleGuard()) compares a fixed grid of its texels
// with every real frame the ring holds: the newest (1 back), the one before it (2 back) and the
// oldest (3 back). This header turns those per-frame counts of equal texels into one verdict.
// See superdoc/features/frame-generation-stale-frames.md.
#pragma once

#include <cstdio>

namespace fghost
{
	namespace stale
	{
		// The ring frames a probe compares with: newest, 2 back, 3 back (the normal ring depth).
		constexpr int kMaxRef = 3;
		// The probe's grid side: kGrid x kGrid texels, spread evenly over the frame.
		constexpr int kGrid = 64;

		enum class Verdict
		{
			NoReference,   // the ring holds no frame yet: nothing to compare with, copy as usual
			Fresh,         // differs from every ring frame: a normal new frame
			Repeat,        // identical to the newest ring frame (a static picture or a plain repeat): used as usual
			Stale,         // identical to an OLDER ring frame and not to the newest: a step back in time, never used
		};

		struct Result
		{
			Verdict eVerdict = Verdict::NoReference;
			int nBack = 0;   // Stale: how many frames back the match is (2 or 3); Repeat: 1; else 0
		};

		// nProbed: texels compared (kGrid * kGrid). pnEqual[i] / pbValid[i]: the texels equal to the ring frame
		// i + 1 arrivals back, and whether that frame exists. A frame counts as "identical" only when EVERY probed
		// texel is equal (the stale frames seen in the field were byte-identical over the whole frame).
		//
		// Why "identical to an older frame AND different from the newest": a game that rotates its swapchain
		// buffers and hands one over again before its new rendering has landed (or without drawing into it)
		// presents the picture that buffer held before, one, two or three frames back; that is a step back in
		// time, and a pair built on it is two pictures that do not follow each other. Identical to the newest
		// as well is a still picture (all three alike) or a plain repeat: shown the same either way, so it is
		// used as before and only counted.
		inline Result Judge( int nProbed, const int *pnEqual, const bool *pbValid, int nRef )
		{
			Result r;
			if ( nProbed <= 0 || nRef <= 0 || !pbValid[ 0 ] )
				return r;
			if ( pnEqual[ 0 ] >= nProbed )
			{
				r.eVerdict = Verdict::Repeat;
				r.nBack = 1;
				return r;
			}
			for ( int i = 1; i < nRef && i < kMaxRef; i++ )
			{
				if ( pbValid[ i ] && pnEqual[ i ] >= nProbed )
				{
					r.eVerdict = Verdict::Stale;
					r.nBack = i + 1;
					return r;
				}
			}
			r.eVerdict = Verdict::Fresh;
			return r;
		}

		// ritz_fg_stale_test <n>: every n-th new real frame (counting from 1) is judged stale on purpose, so the
		// fail-safe path can be exercised where real stale frames never come. n <= 0 = off.
		inline bool ForcedStale( int nEvery, unsigned long long ulNewFrameIndex )
		{
			return nEvery > 0 && ulNewFrameIndex > 0 && ( ulNewFrameIndex % (unsigned long long)nEvery ) == 0;
		}

		// ---- what the compositor knows about the commit a new real frame came from ----
		// Filled by steamcompmgr.cpp's FrameGen_CommitFacts() when the frame is first taken (the probe), logged
		// with a stale verdict or a "changed after it was taken". Tri-state ints: 1 yes, 0 no, -1 unknown.
		struct CommitFacts
		{
			bool bFound = false;          // the commit is still in the focused window's queue
			bool bExplicitSync = false;   // it had an explicit-sync acquire point
			bool bReadyAtCommit = false;  // that point was already signalled when the commit was received
			bool bRecommit = false;       // no new buffer: the same buffer and release point as a commit before it
			int nReleased = -1;           // its release point was already signalled (the game had the buffer back)
			int nWritePending = -1;       // the buffer's implicit write fence was still pending
			double flReceivedToDoneMs = -1.0;   // received -> ready (acquire fence)
			double flDoneToTakenMs = -1.0;      // ready -> the window's newest done commit
			double flTakenToProbeMs = -1.0;     // -> this paint's probe
			unsigned uTexRefs = 0;        // references on the buffer's texture / its backend framebuffer now
			unsigned uFbRefs = 0;
		};

		// A commit whose release point has already been signalled has been handed back to the game: the game may
		// be drawing its next frame into that buffer while the compositor reads it. Point 0 is no point (a
		// timeline starts at 0, so it always reads as signalled).
		inline bool HandedBack( bool bHaveReleasePoint, unsigned long long ulSignalled, unsigned long long ulPoint )
		{
			return bHaveReleasePoint && ulPoint != 0 && ulSignalled >= ulPoint;
		}

		inline const char *TriText( int n, const char *pszYes, const char *pszNo )
		{
			return n > 0 ? pszYes : n == 0 ? pszNo : "n/a";
		}

		// The facts as one log suffix ("commit ...; ..."), never longer than cb - 1 characters.
		inline int FormatFacts( const CommitFacts &f, char *psz, unsigned long cb )
		{
			if ( !psz || cb == 0 )
				return 0;
			if ( !f.bFound )
				return snprintf( psz, cb, "commit gone" );
			return snprintf( psz, cb,
				"commit: %s sync, %s, %s, released to the game %s, write fence %s, received->ready %.2f ms, ready->newest %.2f ms, ->probe %.2f ms, refs %u/%u",
				f.bExplicitSync ? "explicit" : "implicit",
				f.bReadyAtCommit ? "ready at commit" : "waited for",
				f.bRecommit ? "re-commit without a new buffer" : "new buffer",
				TriText( f.nReleased, "YES", "no" ),
				TriText( f.nWritePending, "PENDING", "signalled" ),
				f.flReceivedToDoneMs, f.flDoneToTakenMs, f.flTakenToProbeMs, f.uTexRefs, f.uFbRefs );
		}
	}
}
