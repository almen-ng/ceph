
#include "MDStore.h"
#include "MDS.h"
#include "MDCache.h"
#include "CInode.h"
#include "CDir.h"
#include "CDentry.h"
#include "MDCluster.h"

#include "include/Message.h"

#include <cassert>
#include <iostream>
using namespace std;


#define  dout    if (1) cout << "mds" << mds->get_nodeid() << " "



void MDStore::proc_message(Message *m)
{
  switch (m->get_type()) {

  default:
	dout << "store unknown message " << m->get_type() << endl;
	assert(0);
  }
}



// == fetch_dir


class MDFetchDirContext : public Context {
 protected:
  MDStore *ms;
  inodeno_t ino;

 public:
  char *buf;
  size_t buflen;

  MDFetchDirContext(MDStore *ms, inodeno_t ino) : Context() {
	this->ms = ms;
	this->ino = ino;
	buf = 0;
	buflen = 0;
  }
  ~MDFetchDirContext() {
	if (buf) { delete buf; buf = 0; }
  }
  
  void finish(int result) {
	ms->fetch_dir_2( result, buf, buflen, ino );
  }
};


bool MDStore::fetch_dir( CInode *in,
						 Context *c )
{
  dout << "fetch_dir " << in->inode.ino << endl;
  if (c) 
	in->dir->add_waiter(c);

  // already fetching?
  if (in->mid_fetch) {
	dout << "already fetching " << in->inode.ino << "; waiting" << endl;
	return true;
  }
  in->mid_fetch = true;

  // create return context
  MDFetchDirContext *fin = new MDFetchDirContext( this, in->ino() );

  // issue osd read
  int osd = mds->mdcluster->get_meta_osd(in->inode.ino);
  object_t oid = mds->mdcluster->get_meta_oid(in->inode.ino);
  
  mds->osd_read( osd, oid, 
				 0, 0,
				 &fin->buf, &fin->buflen,
				 fin );
}

bool MDStore::fetch_dir_2( int result, char *buf, size_t buflen, inodeno_t ino)
{
  CInode *idir = mds->mdcache->get_inode(ino);
  if (!idir) {
	dout << *mds << "fetch_dir_2 on ino " << ino << " but no longer in our cache!" << endl;

	delete[] buf; // free buffer
	return false;
  } 

  if (idir->dir_authority(mds->get_cluster()) != mds->get_nodeid()) {

	// oh well
	dout << *mds << "fetch_dir_2 on " << *idir << ", but i'm not the authority." << endl;
	
  } else {

	// do it

	dout << *mds << "fetch_dir_2 on " << *idir << " ref " << idir->ref << " has " << idir->dir->get_size() << endl;
	
	// make sure we have a CDir
	if (idir->dir == NULL) idir->dir = new CDir(idir);
	
	// parse buffer contents into cache
	__uint32_t num = *((__uint32_t*)buf);
	dout << "  " << num << " items" << endl;
	size_t p = 4;
	int parsed = 0;
	while (parsed < num) {
	  assert(p < buflen && num > 0);
	  parsed++;
	  
	  // dentry
	  string dname = buf+p;
	  p += dname.length() + 1;
	  //dout << "parse filename " << dname << endl;
	  
	  // just a hard link?
	  if (*(buf+p) == 'L') {
		// yup.  we don't do that yet.
		assert(0);
	  } else {
		p++;
		
		inode_t *inode = (inode_t*)(buf+p);
		p += sizeof(inode_t);
		
		if (mds->mdcache->have_inode(inode->ino)) {
		  CInode *in = mds->mdcache->get_inode(inode->ino);
		  dout << "readdir got (but i already had) " << *in << " isdir " << in->inode.isdir << " touched " << in->inode.touched<< endl;
		  continue;
		}
		
		// inode
		CInode *in = new CInode();
		memcpy(&in->inode, inode, sizeof(inode_t));
		
		// add and link
		mds->mdcache->add_inode( in );
		mds->mdcache->link_inode( idir, dname, in );
		
		dout << "readdir got " << *in << " isdir " << in->inode.isdir << " touched " << in->inode.touched<< endl;
		
		in->cached_by.clear();
	  
		// HACK
		/*
		  if (idir->inode.ino == 1 && mds->get_nodeid() == 0 && in->is_dir()) {
		  int d = rand() % mds->mdcluster->get_size();
		  if (d > 0) {
		  dout << "hack: exporting dir" << endl;
		  mds->mdcache->export_dir( in, d);
		  }
		  }
		*/
		
	  }
	}

	// dir is now complete
	idir->dir->state_set(CDIR_MASK_COMPLETE);
	
	// trim cache?
	mds->mdcache->trim();
  }

 
  // free buffer
  delete[] buf;
  
  // finish
  list<Context*> finished;
  idir->dir->take_waiting(finished);
  idir->mid_fetch = false;
  
  list<Context*>::iterator it = finished.begin();	
  while (it != finished.end()) {
	Context *c = *(it++);
	if (c) {
	  c->finish(0);
	  delete c;
	}
  }
}








// ----------------------------
// commit_dir

class C_MDS_CommitDirDelay : public Context {
public:
  MDS *mds;
  inodeno_t ino;
  Context *c;
  C_MDS_CommitDirDelay( MDS *mds, inodeno_t ino, Context *c) {
	this->mds = mds;
	this->c = c;
	this->ino = ino;
  }
  virtual void finish(int r) {
	CInode *in = mds->mdcache->get_inode(ino);
	if (in) {
	  mds->mdstore->commit_dir(in, c);
	} else {
	  // must have exported ors omethign!
	  dout << "can't retry commit dir on " << ino << ", must have exported?" << endl;
	  if (c) {
 		c->finish(-1);
		delete c;
	  }
	}
  }
};

class MDCommitDirContext : public Context {
 protected:
  MDStore *ms;
  CInode *in;
  Context *c;
  __uint64_t version;

 public:
  char *buf;
  size_t buflen;

  MDCommitDirContext(MDStore *ms, CInode *in, Context *c) : Context() {
	this->ms = ms;
	this->in = in;
	this->c = c;
	buf = 0;
	buflen = 0;
	version = in->dir->get_version();
  }
  MDCommitDirContext() {
	if (buf) { delete buf; buf = 0; }
  }
  
  void finish(int result) {
	ms->commit_dir_2( result, in, c, version );
  }
};


class MDFetchForCommitContext : public Context {
protected:
  MDStore *ms;
  CInode *in;
  Context *co;
public:
  MDFetchForCommitContext(MDStore *m, CInode *i, Context *c) {
	ms = m; in = i; co = c;
  }
  void finish(int result) {
	ms->commit_dir( in, co );
  }
};



bool MDStore::commit_dir( CInode *in,
						  Context *c )
{
  // already committing?
  if (in->dir->get_state() & CDIR_MASK_MID_COMMIT) {
	// already mid-commit!
	dout << "dir already mid-commit" << endl;
	return false;
  }

  if (!in->dir->can_hard_pin()) {
	// something must be frozen up the hiearchy!
	in->dir->add_hard_pin_waiter( new C_MDS_CommitDirDelay(mds, in->inode.ino, c) );
	return false;
  }


  // is it complete?
  if (in->dir->get_state() & CDIR_MASK_COMPLETE == 0) {
	dout << "dir not complete, fetching first" << endl;
	// fetch dir first
	Context *fin = new MDFetchForCommitContext(this, in, c);
	fetch_dir(in, fin);
	return false;
  }




  // get continuation ready
  MDCommitDirContext *fin = new MDCommitDirContext(this, in, c);
  
  // buffer
  fin->buflen = in->dir->serial_size();
  fin->buf = new char[fin->buflen];
  __uint32_t num = 0;
  size_t off = sizeof(num);

  // fill
  CDir_map_t::iterator it = in->dir->begin();
  while (it != in->dir->end()) {
	// name
	memcpy(fin->buf + off, it->first.c_str(), it->first.length() + 1);
	off += it->first.length() + 1;
	
	// marker
	fin->buf[off++] = 'I';  // inode

	// inode
	memcpy(fin->buf + off, &it->second->get_inode()->inode, sizeof(inode_t));
	off += sizeof(inode_t);

	it++;	
	num++;
  }
  *((__uint32_t*)fin->buf) = num;
  
  // pin inode
  in->dir->hard_pin();
  in->dir->state_set(CDIR_MASK_MID_COMMIT);

  // submit to osd
  int osd = mds->mdcluster->get_meta_osd(in->inode.ino);
  object_t oid = mds->mdcluster->get_meta_oid(in->inode.ino);
  
  mds->osd_write( osd, oid, 
				  off, 0,
				  fin->buf,
				  0,
				  fin );
  return true;
}


bool MDStore::commit_dir_2( int result,
							CInode *in,
							Context *c,
							__uint64_t committed_version)
{
  // is the dir now clean?
  if (committed_version == in->dir->get_version()) {
	in->dir->state_clear(CDIR_MASK_DIRTY);   // clear dirty bit
  }
  in->dir->state_clear(CDIR_MASK_MID_COMMIT);

  // unpin
  in->dir->hard_unpin();

  // finish
  if (c) {
	c->finish(result);
	delete c;
  }

}
