// $Id: RiffMgr.cpp 227 2022-12-25 00:44:45Z duncang $

//=================================================================================================
// Original File Name : RiffMgr.cpp
// Original Author    : duncang
// Creation Date      : Sep 28, 2013
// Copyright          : Copyright © 2011 by Catraeus and Duncan Gray
//
// Description        :
/*

Is a machine/interface for RIFF files.  Since the RIFF has lots and lots of stuff, it provides
  other interfaces.  Some of the tricksiest stuff this does is the number systems that RIFF
  uses to pack data.  At Catraeus, the Stream is ALWAYS double (IEEE 64-bit float)
Is an interface to a Stream, hiding all of the RIFF stuff.
Drives an interface to an AudioFile object.


    Definitions:
      Stream       The musician's currency.  Has FS, Length, Channels.  Contiguous and complete
      Store        The file system's currency.  Has a file descriptor and an encapsulation spec (RIFF::WAVE, fLaC, MP3...
    Use Cases:
      A small file is read in completely to a stream image.
      A small file is written completely from a stream image.
      A large file is read in piece-meal.  String-of-pearl like.  and each pearl is discarded after the musician is done.
        An analyzer will do this to provide a summary analysis or shuttle through for views.
      A large file is randomly read, with an arbitrarily sized, located block.
        An analyzer will do this to shuttle through a viewport of it.  SpecAn, Scope, Song Looping.
    Attributes:
      File Control I/F -
        public:
          File Name
          File Size (in bytes)
          File Type (r, w, rw, b, t)
          File State (enum) GetState() {idle, open, closed, non-existant, isDir, inaccessible} some conflation
          Open (locking, no-locking)
          Close()
          Fetch (n bytes from p location)
          Overwrite (n bytes to p location)
          Append (n bytes)
          <callback>changed out from under us
        private:
          lock, unlock
      Stream I/F - Has info about the RIFF.  In that sense, it is duplicated info between here and a stream.
        public:
          FS
          Len (in samples)
          Len (in time)
          channels
          Start (of in-mem resident sub-stream) in samples
          Len (of in-mem resident sub-stream) in samples
      RIFF I/F
        The RIFF structure on file is flat organized.  RIFF, however, is Hierarchical.  At each GetNext moment, the RiffMgr will simply make a straight-line array of Riff... objects.
          But it also has to figure out whether to push children or pop to a parent to add another child. Hierarcical Tree kind of stuff.
        public:
          Count Number of RIFF sub-sections
          Array of RiffChunk objects There should never be a base class only derived.
          And now is the hard part because, e.g. the user of the RIFF info has to intimately know RIFF.  I'm not going to make
            the RIFF chunk interface expose all of the bits, so the GUI of RIFF has to be really intelligent.
        Methods:
          public:
            Traverse()  Starts at 0 in the file and does a bunch of GetNext().  Tells whether there is residue, leaves a pointer to anything that might come after.
          private:
            GetNext(RiffCk) given the RiffCk, find the next pointer, dig out the fourCC, return a Ck... that is the right kind.
              Since the Ck... got "new"'d it will have auto-parsed.  Ready for doing a GetNext() if there is residue.
      During Traversal, RiffMgr doesn't keep track of the File locations.  Each RiffChunk has knowledge of the file offset and size.
        Assumes that any single RIFF Chunk is contiguous.
        RiffMgr, however, does keep track of RiffCk residue vs. AudioFile residue.
      The Stream Interface interacts in a special way with a RiffChunkData object to get and put data.
    Functions:
      Set File - to associate an AudioFile instance with a RiffFile instance.
      Traverse - to get the stuff on disk into the h tree that is a RIFF smart object.


*/
//
//=================================================================================================

#include <stdio.h>
#include <string.h>
#include <stack>

#include "RiffMgr.hpp"
           RiffMgr::RiffMgr        ( AudioFiler *i_taf, NumSys *i_tns )
: taf(i_taf)
, tns(i_tns) {
  ctAm            = CtlAudMsg ::GetInstance();

  metaBlock       = new byte[65536] ;
  metaSize        = 0;
  riffs           = new RiffCk*[EC_MAX_RIFFS];  for(uint i=0; i<EC_MAX_RIFFS; i++) riffs[i]   = 0;
  offsets         = new uint   [EC_MAX_RIFFS];  for(uint i=0; i<EC_MAX_RIFFS; i++) offsets[i] = 0;
  tcUnk           = new CkUnk *[EC_MAX_RIFFS];  for(uint i=0; i<EC_MAX_RIFFS; i++) tcUnk[i]   = 0;
  numRiffs = EC_MAX_RIFFS;
  numUnk   = EC_MAX_RIFFS;

  CbChangeRiff    = NULL ;
//  MSU_FetchBytes  = NULL ;
//  MSU_GetFileSize = NULL ;

  Reset();


  }
           RiffMgr::~RiffMgr       ( void        ) {
  for(uint i=0; i<numRiffs; i++) {delete riffs[i];}
  for(uint i=0; i<numUnk;   i++) {delete tcUnk[i];}
  delete riffs;   riffs   = 0;
  delete offsets; offsets = 0;
  delete tcUnk;   tcUnk   = 0;
  }
//=================================================================================================
// Interconnect the machines
void       RiffMgr::OnFileUpdateSrc  ( void        ) {
  Reset();
  return;
}
void       RiffMgr::OnParse          ( void        ) {
  byte   *pHdr;
  byte   *pImg;
  ullong  nHdr;
  ullong  nImg;
  ullong  off;
  llong   residue;
  RiffCk *prevCk;
  RiffCk *currCk;
  bool    first;
  sFetch  tSF;
  std::stack<RiffCk *> parents;
/*

So to figure out the push/pop and sibling relationship.
  1. The residue tells you that you're at the end of a level.
  2. The IsHier tells you to go down.
  3. Pictures:
  -        -
   --- -  -
      - -
         -
  ChunkSize - Hier chunks have a size and a boxcar size.  The size is the small meta stuff to announce the hierarchy.
              They then have to have the size of the junk below it.
              Sideways chunks size is their meta plus contents.  Data for example, is a var length while fmt_ is fixed length.
              So really it's all about the residue.
              For sideways, take in residue, subtract ChunkSize, store in your own residue.
              For hier, take in residue, subtract ChunkSize, subtract SubSize, store in your own residue, push.
              Check that residue stays above zero.
              Check that SubSize is not zero.  It's OK but demands preventing a push.
              If residue goes below zero, close the whole thing out immediately but leave what you've found in place with "this" showing invalid.
Figure out family relationships after parse.
  When pushing ... how to tell the new one that it doesn't have siblings?
  When popping, keep checking on the popped for residue.
  check residue at beginning of cycle.
  check "first" (due to a push) at beginning of cycle.
  before parse:
    if residue == 0
      if parents is at top  taken care of by the while
        DONE
        if residue > 0
          doesn't matter ... caller can ask root for residue and see that it's there.  This would be a multi-RIFF or a malformed file.
      else
        pop currCk, no parsing
        unset first // by definition, there was a hier pred
    else // residue not 0
      Just get the next chunk and parse it.
  after parse:
    if not "first"
      set pred/succ for prevCk and currCk
      set this parent equal to pred's parent
    else
      set parent/child using prev or top, kind of doesn't matter.
    if IsHier
      push currCk // yes, the Hier will be called pred for post-pop
      set "first"
    else
      unset "first"

*/

  //== Set up the whole machine
  Reset();
  bool tBool;
  tBool = parents.empty();
  (void)tBool;
  nHdr = RiffCk::PreFetchSize();
  pHdr = metaBlock; // we have to do this here to have a sandbox.  The PreFetchSize is a whole-RIFF World kind of thing, so it's a safe size

  residue = taf->GetFileSize();
  //if(MSU_GetFileSize) MSU_GetFileSize->Execute((void *)(&residue)); else return;

  //== Start the first chunk going
  numRiffs = 0;
  offsets[numRiffs] = 0;
  off = offsets[numRiffs];
  tSF.pImg = pHdr;  tSF.nImg = nHdr;  tSF.off  = off;
  taf->Fetch(tSF.pImg, tSF.nImg, tSF.off);
  //if(MSU_FetchBytes) MSU_FetchBytes->Execute((void *)(&tSF)); else return;     // Harks back up to the AudioFiler. for now, we only do one file.
  currCk  = RiffCk::RiffFactory(pHdr, tns); // damned well better be a RIFF chunk.  FIXME Protect against non-RIFF start
  nImg      = currCk->FetchSize();
  pImg      = (byte *)currCk->GetImageLoc();
  tSF.pImg = pImg;  tSF.nImg = nImg;  tSF.off  = off;
  taf->Fetch(tSF.pImg, tSF.nImg, tSF.off);
  //if(MSU_FetchBytes) MSU_FetchBytes->Execute((void *)(&tSF)); else return;     // for now, we only do one file.
  //== Distribute the packed contents into useable fields of each chunk class
  currCk->ParseBody();
  currCk->SetOrder(numRiffs);
  PlaceCanonical(currCk);
  //== Set up for the next cycle.  We are Hier since this IS a RIFF chunk.
  currCk->SetRes(residue);    // This protects us with respect to residue.
  offsets[numRiffs + 1] = off + currCk->LeafSize();
  riffs[numRiffs] = currCk;
  //== No need for test here since we just know it's a RIFF and is Hier
  parents.push(currCk);
  residue = currCk->GetSubSize();
  first = true; // hierarchy - know that this is the first in a child list.
  currCk->SetFileOff(0);
  numRiffs++;
  prevCk = currCk;
  while(parents.size() > 0) {
    if(residue <= 0) {
      currCk = parents.top();
      parents.pop();
      first = false;
    }
    else {
      //==  Set up the machine to figure out the chunk
      off = offsets[numRiffs];                         // get our offset into the physical file
      tSF.pImg = pHdr;  tSF.nImg = nHdr;  tSF.off  = off;
      taf->Fetch(tSF.pImg, tSF.nImg, tSF.off);
      //if(MSU_FetchBytes) MSU_FetchBytes->Execute((void *)(&tSF)); else return;     // for now, we only do one file.
      currCk = RiffCk::RiffFactory(pHdr, tns);  // Let the factory make us a new Riff object of the right type
      nImg = currCk->FetchSize();                    // Find out, from the one who knows, how much file to get for this exercise
      pImg = (byte *)currCk->GetImageLoc();          // Find out, from the one who knows, where to put the fetched bytes
      tSF.pImg = pImg;  tSF.nImg = nImg;  tSF.off  = off;
      taf->Fetch(tSF.pImg, tSF.nImg, tSF.off);
      //if(MSU_FetchBytes) MSU_FetchBytes->Execute((void *)(&tSF)); else return;     // for now, we only do one file.
      //== Distribute the packed contents into useable fields of each chunk class
      currCk->ParseBody();                           // Let the one who knows parse through its junk to enable the next piece of work
      currCk->SetOrder(numRiffs);
      PlaceCanonical(currCk);
      riffs[numRiffs] = currCk;
      currCk->SetRes(residue);
      //== Set up for the next cycle (if it is needed) -- non-hierarchical stuff
      ullong tOffff;
      tOffff  = currCk->LeafSize();
      tOffff += off;
      offsets[numRiffs + 1] = tOffff;
      currCk->SetFileOff(off);
      //== Now figure hierarchy ... this is where it gets fun.
      numRiffs++;
      if(!first) {
        prevCk->SetSucc(currCk);
        currCk->SetPred(prevCk);
        currCk->SetParent(prevCk->GetParent());
      }
      else {
        prevCk->SetChild(currCk);
        currCk->SetParent(prevCk);
      }
      if(currCk->IsHier()) {
        parents.push(currCk);
        first = true;
        residue = currCk->GetSubSize();
      }
      else {
        first = false;
        residue = currCk->GetRes();
      }
      prevCk = currCk;
    }
  }
  valid = true;
  rtsCurr = riffs[0];
  N  = tcData->GetSize();
  N /= tcFmt->GetBlkAlign();
  if(CbChangeRiff != 0)
    CbChangeRiff->Execute(0);
  return;
}
void       RiffMgr::OnCreate         ( llong i_ch, double i_FS) {
  byte   *pOff;
  llong n;
  pOff = metaBlock;
  numRiffs = 0;
  offsets[numRiffs] = 0;
  tcRiff   = new CkRIFF(pOff, RiffCk::ED_WRITE, tns);
      tcRiff->BlankBody();
      PlaceCanonical(tcRiff);
      riffs[numRiffs] = tcRiff;
      n = tcRiff->FetchSize();
      pOff += (size_t)n;
      offsets[numRiffs + 1] = offsets[numRiffs] + n;
      numRiffs++;
  tcFmt    = new CkFmt (pOff, RiffCk::ED_WRITE, tns);
      PlaceCanonical(tcFmt);
      riffs[numRiffs] = tcFmt;
      tcFmt ->BlankBody(i_ch, i_FS );
      n = tcFmt ->FetchSize();
      pOff += (size_t)n;
      offsets[numRiffs + 1] = offsets[numRiffs] + n;
      numRiffs++;
  tcData   = new CkData(pOff, RiffCk::ED_WRITE, tns);
      PlaceCanonical(tcData);
      riffs[numRiffs] = tcData;
      tcData->BlankBody(           );
      n = tcData->FetchSize();
      pOff += (size_t)n;
      offsets[numRiffs + 1] = offsets[numRiffs] + n;
      numRiffs++;
  metaSize  = tcRiff->LeafSize();
  metaSize += tcFmt->LeafSize();
  metaSize += tcData->FetchSize();
  return;
}
//=================================================================================================

// The RIFF tree.  Hide the RIFF, but let outsiders traverse it.
bool       RiffMgr::RiffTreeReRoot   ( void        ) {
  rtsCurr = riffs[0];
  return true;
}
//=================================================================================================
// Particulate Matter that is appropriate for this class
void       RiffMgr::Reset            ( void        ) {
  for(uint i=0; i<numRiffs; i++)
    if(riffs[i] != 0)
      delete riffs[i];
  for(uint i=0; i<EC_MAX_RIFFS; i++)
    riffs[i]   = 0;
  for(uint i=0; i<numUnk; i++)
    if(tcUnk[i] != 0)    delete tcUnk[i];
  // WARNING -- Since the above has done the delete for all RIFF objects we possibly could have created, the following is OK
  for(uint i=0; i<EC_MAX_RIFFS; i++) {
    tcUnk[i]   = 0;
    offsets[i] = 0;
  }
  numUnk    = 0;
  numRiffs  = 0;
  tcRiff    = NULL;
  tcBext    = NULL;
  tcPad     = NULL;
  tcFmt     = NULL;
  tcFact    = NULL;
  tcData    = NULL;
  tcInfo    = NULL;
  tcPeak    = NULL;
  rtsCurr   = NULL;
  // END OF WARNING no
  remainder = 0;


  valid       = false;
  dirty       = true;
  stillGood   = false;

  return;
}
void       RiffMgr::PlaceCanonical   ( RiffCk         *i_currRiff  ) {
  char *tStr;
  tStr = (char *)i_currRiff->GetHdr();
       if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_RIFF)) == 0)  tcRiff          = (CkRIFF *)i_currRiff;
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_WAVE)) == 0)  tcUnk[numUnk++] = (CkUnk  *)i_currRiff;
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_FLAC)) == 0)  tcUnk[numUnk++] = (CkUnk  *)i_currRiff;
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_bext)) == 0)  tcBext          = (CkBext *)i_currRiff;
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_fmt_)) == 0) {tcFmt           = (CkFmt  *)i_currRiff; tcFmt->SetNS(tns);}
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_fact)) == 0)  tcFact          = (CkFact *)i_currRiff;
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_PAD_)) == 0)  tcPad           = (CkPAD  *)i_currRiff;
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_JUNK)) == 0)  tcUnk[numUnk++] = (CkUnk  *)i_currRiff;
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_cue_)) == 0)  tcUnk[numUnk++] = (CkUnk  *)i_currRiff;
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_wavl)) == 0)  tcUnk[numUnk++] = (CkUnk  *)i_currRiff;
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_data)) == 0)  tcData          = (CkData *)i_currRiff;
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_slnt)) == 0)  tcUnk[numUnk++] = (CkUnk  *)i_currRiff;
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_list)) == 0)  tcUnk[numUnk++] = (CkUnk  *)i_currRiff;
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_info)) == 0)  tcInfo          = (CkInfo *)i_currRiff;
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_PEAK)) == 0)  tcPeak          = (CkPEAK *)i_currRiff;
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_labl)) == 0)  tcUnk[numUnk++] = (CkUnk  *)i_currRiff;
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_note)) == 0)  tcUnk[numUnk++] = (CkUnk  *)i_currRiff;
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_ltxt)) == 0)  tcUnk[numUnk++] = (CkUnk  *)i_currRiff;
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_plst)) == 0)  tcUnk[numUnk++] = (CkUnk  *)i_currRiff;
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_smpl)) == 0)  tcUnk[numUnk++] = (CkUnk  *)i_currRiff;
  else if(strcmp(tStr, RiffCk::GetForm4CC(RiffCk::ERF_inst)) == 0)  tcUnk[numUnk++] = (CkUnk  *)i_currRiff;
  else                                                              tcUnk[numUnk++] = (CkUnk  *)i_currRiff;

  return;
}
void       RiffMgr::FmtSetType       ( NumSys::eType  i_type  ) {
  ullong b;

  tcFmt->SetType(i_type);

  b  = tcFmt->GetBlkAlign();
  b *= N;
  tcData->SetSize(b);
  return;
  }
ullong     RiffMgr::FmtSetCh         ( ullong           i_ch        ) {
  ullong b;
  b  = tcFmt->SetCh(i_ch);
  b  = tcFmt->GetBlkAlign();
  b *= N;
  tcData->SetSize(b);
  return tcFmt->GetCh();
  }

llong      RiffMgr::SetN             ( llong           i_N          ) {
  llong b;

  if(i_N > 1000000000) i_N = 1000000000;
  if(i_N <          1) i_N = 1;
  N = i_N;
  b = N * tcFmt->GetBlkAlign();
  tcData->SetSize(b);
  return N;
  }
llong      RiffMgr::SetBitDepth      ( llong           i_bitdepth  ) {
  return tcFmt->GetBitDepth();
  }
double     RiffMgr::FmtSetFS         ( double          i_FS        ) {
  return tcFmt->SetFS(i_FS);
  }
void       RiffMgr::Build            ( void                        )  {
  uint riffSize;
  riffSize = metaSize;           // All header stuff up to the beginning of the data
  riffSize -= RiffCk::FORM_SIZE; // Because the subSize mustn't include theh RIFF header byte count
  riffSize -= RiffCk::FORM_SIZE; // Because the subSize mustn't include theh RIFF subSize byte count
  riffSize += tcData->GetSize(); // Because it will include the size of the audio data
  tcRiff->SetSubSize(riffSize);
  return;
  }
llong      RiffMgr::GetFileSize      ( void                        ) {
  llong l;
  llong d;
  l  = tcRiff->FetchSize();
  d = tcFmt->FetchSize();
  l += d;
  d = tcData->FetchSize();
  l += d;
  d = tcData->GetSize();
  l += d;
  return l;
}
