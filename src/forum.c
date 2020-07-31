/*
** Copyright (c) 2018 D. Richard Hipp
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the Simplified BSD License (also
** known as the "2-Clause License" or "FreeBSD License".)
**
** This program is distributed in the hope that it will be useful,
** but without any warranty; without even the implied warranty of
** merchantability or fitness for a particular purpose.
**
** Author contact information:
**   drh@hwaci.com
**   http://www.hwaci.com/drh/
**
*******************************************************************************
**
** This file contains code used to generate the user forum.
*/
#include "config.h"
#include <assert.h>
#include "forum.h"

/*
** Default to using Markdown markup
*/
#define DEFAULT_FORUM_MIMETYPE  "text/x-markdown"

#if INTERFACE
/*
** Each instance of the following object represents a single message - 
** either the initial post, an edit to a post, a reply, or an edit to
** a reply.
*/
struct ForumEntry {
  int fpid;              /* rid for this entry */
  int fprev;             /* zero if initial entry.  non-zero if an edit */
  int firt;              /* This entry replies to firt */
  int mfirt;             /* Root in-reply-to */
  int nReply;            /* Number of replies to this entry */
  int sid;               /* Serial ID number */
  char *zUuid;           /* Artifact hash */
  ForumEntry *pLeaf;     /* Most recent edit for this entry */
  ForumEntry *pEdit;     /* This entry is an edit of pEdit */
  ForumEntry *pNext;     /* Next in chronological order */
  ForumEntry *pPrev;     /* Previous in chronological order */
  ForumEntry *pDisplay;  /* Next in display order */
  int nIndent;           /* Number of levels of indentation for this entry */
};

/*
** A single instance of the following tracks all entries for a thread.
*/
struct ForumThread {
  ForumEntry *pFirst;    /* First entry in chronological order */
  ForumEntry *pLast;     /* Last entry in chronological order */
  ForumEntry *pDisplay;  /* Entries in display order */
  ForumEntry *pTail;     /* Last on the display list */
  int mxIndent;          /* Maximum indentation level */
};
#endif /* INTERFACE */

/*
** Return true if the forum entry with the given rid has been
** subsequently edited.
*/
int forum_rid_has_been_edited(int rid){
  static Stmt q;
  int res;
  db_static_prepare(&q,
     "SELECT 1 FROM forumpost A, forumpost B"
     " WHERE A.fpid=$rid AND B.froot=A.froot AND B.fprev=$rid"
  );
  db_bind_int(&q, "$rid", rid);
  res = db_step(&q)==SQLITE_ROW;
  db_reset(&q);
  return res;
}

/*
** Delete a complete ForumThread and all its entries.
*/
static void forumthread_delete(ForumThread *pThread){
  ForumEntry *pEntry, *pNext;
  for(pEntry=pThread->pFirst; pEntry; pEntry = pNext){
    pNext = pEntry->pNext;
    fossil_free(pEntry->zUuid);
    fossil_free(pEntry);
  }
  fossil_free(pThread);
}

#if 0 /* not used */
/*
** Search a ForumEntry list forwards looking for the entry with fpid
*/
static ForumEntry *forumentry_forward(ForumEntry *p, int fpid){
  while( p && p->fpid!=fpid ) p = p->pNext;
  return p;
}
#endif

/*
** Search backwards for a ForumEntry
*/
static ForumEntry *forumentry_backward(ForumEntry *p, int fpid){
  while( p && p->fpid!=fpid ) p = p->pPrev;
  return p;
}

/*
** Add an entry to the display list
*/
static void forumentry_add_to_display(ForumThread *pThread, ForumEntry *p){
  if( pThread->pDisplay==0 ){
    pThread->pDisplay = p;
  }else{
    pThread->pTail->pDisplay = p;
  }
  pThread->pTail = p;
}

/*
** Extend the display list for pThread by adding all entries that
** reference fpid.  The first such entry will be no earlier then
** entry "p".
*/
static void forumthread_display_order(
  ForumThread *pThread,    /* The complete thread */
  ForumEntry *pBase        /* Add replies to this entry */
){
  ForumEntry *p;
  ForumEntry *pPrev = 0;
  for(p=pBase->pNext; p; p=p->pNext){
    if( p->fprev==0 && p->mfirt==pBase->fpid ){
      if( pPrev ){
        pPrev->nIndent = pBase->nIndent + 1;
        forumentry_add_to_display(pThread, pPrev);
        forumthread_display_order(pThread, pPrev);
      }
      pBase->nReply++;
      pPrev = p;
    }
  }
  if( pPrev ){
    pPrev->nIndent = pBase->nIndent + 1;
    if( pPrev->nIndent>pThread->mxIndent ) pThread->mxIndent = pPrev->nIndent;
    forumentry_add_to_display(pThread, pPrev);
    forumthread_display_order(pThread, pPrev);
  }
}

/*
** Construct a ForumThread object given the root record id.
*/
static ForumThread *forumthread_create(int froot, int computeHierarchy){
  ForumThread *pThread;
  ForumEntry *pEntry;
  Stmt q;
  int sid = 1;
  Bag seen = Bag_INIT;
  pThread = fossil_malloc( sizeof(*pThread) );
  memset(pThread, 0, sizeof(*pThread));
  db_prepare(&q,
     "SELECT fpid, firt, fprev, (SELECT uuid FROM blob WHERE rid=fpid)"
     "  FROM forumpost"
     " WHERE froot=%d ORDER BY fmtime",
     froot
  );
  while( db_step(&q)==SQLITE_ROW ){
    pEntry = fossil_malloc( sizeof(*pEntry) );
    memset(pEntry, 0, sizeof(*pEntry));
    pEntry->fpid = db_column_int(&q, 0);
    pEntry->firt = db_column_int(&q, 1);
    pEntry->fprev = db_column_int(&q, 2);
    pEntry->zUuid = fossil_strdup(db_column_text(&q,3));
    pEntry->mfirt = pEntry->firt;
    pEntry->sid = sid++;
    pEntry->pPrev = pThread->pLast;
    pEntry->pNext = 0;
    bag_insert(&seen, pEntry->fpid);
    if( pThread->pLast==0 ){
      pThread->pFirst = pEntry;
    }else{
      pThread->pLast->pNext = pEntry;
    }
    if( pEntry->firt && !bag_find(&seen,pEntry->firt) ){
      pEntry->firt = froot;
      pEntry->mfirt = froot;
    }
    pThread->pLast = pEntry;
  }
  db_finalize(&q);
  bag_clear(&seen);

  /* Establish which entries are the latest edit.  After this loop
  ** completes, entries that have non-NULL pLeaf should not be
  ** displayed.
  */
  for(pEntry=pThread->pFirst; pEntry; pEntry=pEntry->pNext){
    if( pEntry->fprev ){
      ForumEntry *pBase = 0, *p;
      p = forumentry_backward(pEntry->pPrev, pEntry->fprev);
      pEntry->pEdit = p;
      while( p ){
        pBase = p;
        p->pLeaf = pEntry;
        p = pBase->pEdit;
      }
      for(p=pEntry->pNext; p; p=p->pNext){
        if( p->mfirt==pEntry->fpid ) p->mfirt = pBase->fpid;
      }
    }
  }

  if( computeHierarchy ){
    /* Compute the hierarchical display order */
    pEntry = pThread->pFirst;
    pEntry->nIndent = 1;
    pThread->mxIndent = 1;
    forumentry_add_to_display(pThread, pEntry);
    forumthread_display_order(pThread, pEntry);
  }

  /* Return the result */
  return pThread;
}

/*
** List all forum threads to standard output.
*/
static void forum_thread_list(void){
  Stmt q;
  db_prepare(&q,
    " SELECT"
    "  datetime(max(fmtime)),"
    "  sum(fprev IS NULL),"
    "  froot"
    " FROM forumpost"
    " GROUP BY froot"
    " ORDER BY 1;"
  );
  fossil_print("    id  cnt    most recent post\n");
  fossil_print("------ ---- -------------------\n");
  while( db_step(&q)==SQLITE_ROW ){
    fossil_print("%6d %4d %s\n",
      db_column_int(&q, 2),
      db_column_int(&q, 1),
      db_column_text(&q, 0)
    );
  }
  db_finalize(&q);
}

/*
** COMMAND: test-forumthread
**
** Usage: %fossil test-forumthread [THREADID]
**
** Display a summary of all messages on a thread THREADID.  If the
** THREADID argument is omitted, then show a list of all threads.
**
** This command is intended for testing an analysis only.
*/
void forumthread_cmd(void){
  int fpid;
  int froot;
  const char *zName;
  ForumThread *pThread;
  ForumEntry *p;

  db_find_and_open_repository(0,0);
  verify_all_options();
  if( g.argc==2 ){
    forum_thread_list();
    return;
  }
  if( g.argc!=3 ) usage("THREADID");
  zName = g.argv[2];
  fpid = symbolic_name_to_rid(zName, "f");
  if( fpid<=0 ){
    fpid = db_int(0, "SELECT rid FROM blob WHERE rid=%d", atoi(zName));
  }
  if( fpid<=0 ){
    fossil_fatal("Unknown or ambiguous forum id: \"%s\"", zName);
  }
  froot = db_int(0, "SELECT froot FROM forumpost WHERE fpid=%d", fpid);
  if( froot==0 ){
    fossil_fatal("Not a forum post: \"%s\"", zName);
  }
  fossil_print("fpid  = %d\n", fpid);
  fossil_print("froot = %d\n", froot);
  pThread = forumthread_create(froot, 1);
  fossil_print("Chronological:\n");
  fossil_print(
/* 0         1         2         3         4         5         6         7    */
/*  123456789 123456789 123456789 123456789 123456789 123456789 123456789 123 */
  " sid      fpid      firt     fprev     mfirt     pLeaf  nReply  hash\n");
  for(p=pThread->pFirst; p; p=p->pNext){
    fossil_print("%4d %9d %9d %9d %9d %9d  %6d  %8.8s\n", p->sid,
       p->fpid, p->firt, p->fprev, p->mfirt, p->pLeaf ? p->pLeaf->fpid : 0,
       p->nReply, p->zUuid);
  }
  fossil_print("\nDisplay\n");
  for(p=pThread->pDisplay; p; p=p->pDisplay){
    fossil_print("%*s", (p->nIndent-1)*3, "");
    if( p->pLeaf ){
      fossil_print("%d->%d\n", p->fpid, p->pLeaf->fpid);
    }else{
      fossil_print("%d\n", p->fpid);
    }
  }
  forumthread_delete(pThread);
}

/*
** Render a forum post for display
*/
void forum_render(
  const char *zTitle,         /* The title.  Might be NULL for no title */
  const char *zMimetype,      /* Mimetype of the message */
  const char *zContent,       /* Content of the message */
  const char *zClass,         /* Put in a <div> if not NULL */
  int bScroll                 /* Large message content scrolls if true */
){
  if( zClass ){
    @ <div class='%s(zClass)'>
  }
  if( zTitle ){
    if( zTitle[0] ){
      @ <h1>%h(zTitle)</h1>
    }else{
      @ <h1><i>Deleted</i></h1>
    }
  }
  if( zContent && zContent[0] ){
    Blob x;
    if( bScroll ){
      @ <div class='forumPostBody'>
    }else{
      @ <div class='forumPostFullBody'>
    }
    blob_init(&x, 0, 0);
    blob_append(&x, zContent, -1);
    safe_html_context(DOCSRC_FORUM);
    wiki_render_by_mimetype(&x, zMimetype);
    blob_reset(&x);
    @ </div>
  }else{
    @ <i>Deleted</i>
  }
  if( zClass ){
    @ </div>
  }
}

/*
** Generate the buttons in the display that allow a forum supervisor to
** mark a user as trusted.  Only do this if:
**
**   (1)  The poster is an individual, not a special user like "anonymous"
**   (2)  The current user has Forum Supervisor privilege
*/
static void generateTrustControls(Manifest *pPost){
  if( !g.perm.AdminForum ) return;
  if( login_is_special(pPost->zUser) ) return;
  @ <br>
  @ <label><input type="checkbox" name="trust">
  @ Trust user "%h(pPost->zUser)"
  @ so that future posts by "%h(pPost->zUser)" do not require moderation.
  @ </label>
  @ <input type="hidden" name="trustuser" value="%h(pPost->zUser)">
}

/*
** Compute a display name from a login name.
**
** If the input login is found in the USER table, then check the USER.INFO
** field to see if it has display-name followed by an email address.
** If it does, that becomes the new display name.  If not, let the display
** name just be the login.
**
** Space to hold the returned name is obtained from fossil_strdup() or
** mprintf() and should be freed by the caller.
*/
char *display_name_from_login(const char *zLogin){
  static Stmt q;
  char *zResult;
  db_static_prepare(&q,
     "SELECT display_name(info) FROM user WHERE login=$login"
  );
  db_bind_text(&q, "$login", zLogin);
  if( db_step(&q)==SQLITE_ROW && db_column_type(&q,0)==SQLITE_TEXT ){
    const char *zDisplay = db_column_text(&q,0);
    if( fossil_strcmp(zDisplay,zLogin)==0 ){
      zResult = fossil_strdup(zLogin);
    }else{
      zResult = mprintf("%s (%s)", zDisplay, zLogin);
    }
  }else{
    zResult = fossil_strdup(zLogin);
  }
  db_reset(&q);
  return zResult;
}

/*
** Display all posts in a forum thread in chronological order
*/
static void forum_display_chronological(int froot, int target, int bRawMode){
  ForumThread *pThread = forumthread_create(froot, 0);
  ForumEntry *p;
  int notAnon = login_is_individual();
  char cMode = bRawMode ? 'r' : 'c';
  for(p=pThread->pFirst; p; p=p->pNext){
    char *zDate;
    Manifest *pPost;
    int isPrivate;        /* True for posts awaiting moderation */
    int sameUser;         /* True if author is also the reader */
    const char *zUuid;
    char *zDisplayName;   /* The display name */
    int sid;

    pPost = manifest_get(p->fpid, CFTYPE_FORUM, 0);
    if( pPost==0 ) continue;
    if( p->fpid==target ){
      @ <div id="forum%d(p->fpid)" class="forumTime forumSel">
    }else if( p->pLeaf!=0 ){
      @ <div id="forum%d(p->fpid)" class="forumTime forumObs">
    }else{
      @ <div id="forum%d(p->fpid)" class="forumTime">
    }
    if( pPost->zThreadTitle ){
      @ <h1>%h(pPost->zThreadTitle)</h1>
    }
    zDate = db_text(0, "SELECT datetime(%.17g)", pPost->rDate);
    zDisplayName = display_name_from_login(pPost->zUser);
    sid = p->pEdit ? p->pEdit->sid : p->sid;
    @ <h3 class='forumPostHdr'>(%d(sid)) By %h(zDisplayName) on %h(zDate)
    fossil_free(zDisplayName);
    fossil_free(zDate);
    if( p->pEdit ){
      @ edit of %z(href("%R/forumpost/%S?t=%c",p->pEdit->zUuid,cMode))\
      @ %d(p->pEdit->sid)</a>
    }
    if( g.perm.Debug ){
      @ <span class="debug">\
      @ <a href="%R/artifact/%h(p->zUuid)">(artifact-%d(p->fpid))</a></span>
    }
    if( p->firt ){
      ForumEntry *pIrt = p->pPrev;
      while( pIrt && pIrt->fpid!=p->firt ) pIrt = pIrt->pPrev;
      if( pIrt ){
        @ in reply to %z(href("%R/forumpost/%S?t=%c",pIrt->zUuid,cMode))\
        @ %d(pIrt->sid)</a>
      }
    }
    zUuid = p->zUuid;
    if( p->pLeaf ){
      @ updated by %z(href("%R/forumpost/%S?t=%c",p->pLeaf->zUuid,cMode))\
      @ %d(p->pLeaf->sid)</a>
      zUuid = p->pLeaf->zUuid;
    }
    if( p->fpid!=target ){
      @ %z(href("%R/forumpost/%S?t=%c",zUuid,cMode))[link]</a>
    }
    if( !bRawMode ){
      @ %z(href("%R/forumpost/%S?raw",zUuid))[source]</a>
    }
    isPrivate = content_is_private(p->fpid);
    sameUser = notAnon && fossil_strcmp(pPost->zUser, g.zLogin)==0;
    @ </h3>
    if( isPrivate && !g.perm.ModForum && !sameUser ){
      @ <p><span class="modpending">Awaiting Moderator Approval</span></p>
    }else{
      const char *zMimetype;
      if( bRawMode ){
        zMimetype = "text/plain";
      }else if( p->pLeaf!=0 ){
        zMimetype = "text/plain";
      }else{
        zMimetype = pPost->zMimetype;
      }
      forum_render(0, zMimetype, pPost->zWiki, 0, 1);
    }
    if( g.perm.WrForum && p->pLeaf==0 ){
      int sameUser = login_is_individual()
                     && fossil_strcmp(pPost->zUser, g.zLogin)==0;
      @ <div><form action="%R/forumedit" method="POST">
      @ <input type="hidden" name="fpid" value="%s(p->zUuid)">
      if( !isPrivate ){
        /* Reply and Edit are only available if the post has already
        ** been approved */
        @ <input type="submit" name="reply" value="Reply">
        if( g.perm.Admin || sameUser ){
          @ <input type="submit" name="edit" value="Edit">
          @ <input type="submit" name="nullout" value="Delete">
        }
      }else if( g.perm.ModForum ){
        /* Provide moderators with moderation buttons for posts that
        ** are pending moderation */
        @ <input type="submit" name="approve" value="Approve">
        @ <input type="submit" name="reject" value="Reject">
        generateTrustControls(pPost);
      }else if( sameUser ){
        /* A post that is pending moderation can be deleted by the
        ** person who originally submitted the post */
        @ <input type="submit" name="reject" value="Delete">
      }
      @ </form></div>
    }
    manifest_destroy(pPost);
    @ </div>
  }

  /* Undocumented "threadtable" query parameter causes thread table
  ** to be displayed for debugging purposes.
  */
  if( PB("threadtable") ){
    @ <hr>
    @ <table border="1" cellpadding="3" cellspacing="0">
    @ <tr><th>sid<th>fpid<th>firt<th>fprev<th>mfirt<th>pLeaf<th>nReply<th>hash
    for(p=pThread->pFirst; p; p=p->pNext){
      @ <tr><td>%d(p->sid)<td>%d(p->fpid)<td>%d(p->firt)\
      @ <td>%d(p->fprev)<td>%d(p->mfirt)\
      @ <td>%d(p->pLeaf?p->pLeaf->fpid:0)<td>%d(p->nReply)\
      @ <td>%S(p->zUuid)</tr>
    }
    @ </table>
  }

  forumthread_delete(pThread);
}
/*
** Display all the edit history of post "target".
*/
static void forum_display_history(int froot, int target, int bRawMode){
  ForumThread *pThread = forumthread_create(froot, 0);
  ForumEntry *p;
  int notAnon = login_is_individual();
  char cMode = bRawMode ? 'r' : 'c';
  ForumEntry *pLeaf = 0;
  int cnt = 0;
  for(p=pThread->pFirst; p; p=p->pNext){
    if( p->fpid==target ){
      pLeaf = p->pLeaf ? p->pLeaf : p;
      break;
    }
  }
  for(p=pThread->pFirst; p; p=p->pNext){
    char *zDate;
    Manifest *pPost;
    int isPrivate;        /* True for posts awaiting moderation */
    int sameUser;         /* True if author is also the reader */
    const char *zUuid;
    char *zDisplayName;   /* The display name */

    if( p->fpid!=pLeaf->fpid && p->pLeaf!=pLeaf ) continue;
    cnt++;
    pPost = manifest_get(p->fpid, CFTYPE_FORUM, 0);
    if( pPost==0 ) continue;
    @ <div id="forum%d(p->fpid)" class="forumTime">
    zDate = db_text(0, "SELECT datetime(%.17g)", pPost->rDate);
    zDisplayName = display_name_from_login(pPost->zUser);
    @ <h3 class='forumPostHdr'>(%d(p->sid)) By %h(zDisplayName) on %h(zDate)
    fossil_free(zDisplayName);
    fossil_free(zDate);
    if( g.perm.Debug ){
      @ <span class="debug">\
      @ <a href="%R/artifact/%h(p->zUuid)">(artifact-%d(p->fpid))</a></span>
    }
    if( p->firt && cnt==1 ){
      ForumEntry *pIrt = p->pPrev;
      while( pIrt && pIrt->fpid!=p->firt ) pIrt = pIrt->pPrev;
      if( pIrt ){
        @ in reply to %z(href("%R/forumpost/%S?t=%c",pIrt->zUuid,cMode))\
        @ %d(pIrt->sid)</a>
      }
    }
    zUuid = p->zUuid;
    @ %z(href("%R/forumpost/%S?t=c",zUuid))[link]</a>
    if( !bRawMode ){
      @ %z(href("%R/forumpost/%S?raw",zUuid))[source]</a>
    }
    isPrivate = content_is_private(p->fpid);
    sameUser = notAnon && fossil_strcmp(pPost->zUser, g.zLogin)==0;
    @ </h3>
    if( isPrivate && !g.perm.ModForum && !sameUser ){
      @ <p><span class="modpending">Awaiting Moderator Approval</span></p>
    }else{
      forum_render(0, bRawMode?"text/plain":pPost->zMimetype, pPost->zWiki,
                   0, 1);
    }
    if( g.perm.WrForum && p->pLeaf==0 ){
      int sameUser = login_is_individual()
                     && fossil_strcmp(pPost->zUser, g.zLogin)==0;
      @ <div><form action="%R/forumedit" method="POST">
      @ <input type="hidden" name="fpid" value="%s(p->zUuid)">
      if( !isPrivate ){
        /* Reply and Edit are only available if the post has already
        ** been approved */
        @ <input type="submit" name="reply" value="Reply">
        if( g.perm.Admin || sameUser ){
          @ <input type="submit" name="edit" value="Edit">
          @ <input type="submit" name="nullout" value="Delete">
        }
      }else if( g.perm.ModForum ){
        /* Provide moderators with moderation buttons for posts that
        ** are pending moderation */
        @ <input type="submit" name="approve" value="Approve">
        @ <input type="submit" name="reject" value="Reject">
        generateTrustControls(pPost);
      }else if( sameUser ){
        /* A post that is pending moderation can be deleted by the
        ** person who originally submitted the post */
        @ <input type="submit" name="reject" value="Delete">
      }
      @ </form></div>
    }
    manifest_destroy(pPost);
    @ </div>
  }
  forumthread_delete(pThread);
}

/*
** Display all messages in a forumthread with indentation.
*/
static int forum_display_hierarchical(int froot, int target){
  ForumThread *pThread;
  ForumEntry *p;
  Manifest *pPost, *pOPost;
  int fpid;
  const char *zUuid;
  char *zDate;
  const char *zSel;
  int notAnon = login_is_individual();
  int iIndentScale = 4;

  pThread = forumthread_create(froot, 1);
  for(p=pThread->pFirst; p; p=p->pNext){
    if( p->fpid==target ){
      while( p->pEdit ) p = p->pEdit;
      target = p->fpid;
      break;
    }
  }
  while( iIndentScale>1 && iIndentScale*pThread->mxIndent>25 ){
    iIndentScale--;
  }
  for(p=pThread->pDisplay; p; p=p->pDisplay){
    int isPrivate;         /* True for posts awaiting moderation */
    int sameUser;          /* True if reader is also the poster */
    char *zDisplayName;    /* User name to be displayed */
    pOPost = manifest_get(p->fpid, CFTYPE_FORUM, 0);
    if( p->pLeaf ){
      fpid = p->pLeaf->fpid;
      zUuid = p->pLeaf->zUuid;
      pPost = manifest_get(fpid, CFTYPE_FORUM, 0);
    }else{
      fpid = p->fpid;
      zUuid = p->zUuid;
      pPost = pOPost;
    }
    zSel = p->fpid==target ? " forumSel" : "";
    if( p->nIndent==1 ){
      @ <div id='forum%d(fpid)' class='forumHierRoot%s(zSel)'>
    }else{
      @ <div id='forum%d(fpid)' class='forumHier%s(zSel)' \
      @ style='margin-left: %d((p->nIndent-1)*iIndentScale)ex;'>
    }
    if( pPost==0 ) continue;
    if( pPost->zThreadTitle ){
      @ <h1>%h(pPost->zThreadTitle)</h1>
    }
    zDate = db_text(0, "SELECT datetime(%.17g)", pOPost->rDate);
    zDisplayName = display_name_from_login(pOPost->zUser);
    @ <h3 class='forumPostHdr'>\
    @ (%d(p->sid)) By %h(zDisplayName) on %h(zDate)
    fossil_free(zDisplayName);
    fossil_free(zDate);
    if( g.perm.Debug ){
      @ <span class="debug">\
      @ <a href="%R/artifact/%h(p->zUuid)">(artifact-%d(p->fpid))</a></span>
    }
    if( p->pLeaf ){
      zDate = db_text(0, "SELECT datetime(%.17g)", pPost->rDate);
      if( fossil_strcmp(pOPost->zUser,pPost->zUser)==0 ){
        @ and edited on %h(zDate)
      }else{
        @ as edited by %h(pPost->zUser) on %h(zDate)
      }
      fossil_free(zDate);
      if( g.perm.Debug ){
        @ <span class="debug">\
        @ <a href="%R/artifact/%h(p->pLeaf->zUuid)">\
        @ (artifact-%d(p->pLeaf->fpid))</a></span>
      }
      @ %z(href("%R/forumpost/%S?t=y",p->zUuid))[history]</a>
      manifest_destroy(pOPost);
    }
    if( fpid!=target ){
      @ %z(href("%R/forumpost/%S",zUuid))[link]</a>
    }
    @ %z(href("%R/forumpost/%S?raw",zUuid))[source]</a>
    if( p->firt ){
      ForumEntry *pIrt = p->pPrev;
      while( pIrt && pIrt->fpid!=p->mfirt ) pIrt = pIrt->pPrev;
      if( pIrt ){
        @ in reply to %z(href("%R/forumpost/%S?t=h",pIrt->zUuid))\
        @ %d(pIrt->sid)</a>
      }
    }
    @ </h3>
    isPrivate = content_is_private(fpid);
    sameUser = notAnon && fossil_strcmp(pPost->zUser, g.zLogin)==0;
    if( isPrivate && !g.perm.ModForum && !sameUser ){
      @ <p><span class="modpending">Awaiting Moderator Approval</span></p>
    }else{
      forum_render(0, pPost->zMimetype, pPost->zWiki, 0, 1);
    }
    if( g.perm.WrForum ){
      @ <div><form action="%R/forumedit" method="POST">
      @ <input type="hidden" name="fpid" value="%s(zUuid)">
      if( !isPrivate ){
        /* Reply and Edit are only available if the post has already
        ** been approved */
        @ <input type="submit" name="reply" value="Reply">
        if( g.perm.Admin || sameUser ){
          @ <input type="submit" name="edit" value="Edit">
          @ <input type="submit" name="nullout" value="Delete">
        }
      }else if( g.perm.ModForum ){
        /* Provide moderators with moderation buttons for posts that
        ** are pending moderation */
        @ <input type="submit" name="approve" value="Approve">
        @ <input type="submit" name="reject" value="Reject">
        generateTrustControls(pPost);
      }else if( sameUser ){
        /* A post that is pending moderation can be deleted by the
        ** person who originally submitted the post */
        @ <input type="submit" name="reject" value="Delete">
      }
      @ </form></div>
    }
    manifest_destroy(pPost);
    @ </div>
  }
  forumthread_delete(pThread);
  return target;
}

/*
** The first time this is called, it emits SCRIPT tags to load various
** forum-related JavaScript. Ideally it should be called near the end
** of the page, immediately before the call to style_footer() (which
** closes the document's <BODY> and <HTML> tags). Calls after the first
** are a no-op.
*/
static void forum_emit_page_js(){
  static int once = 0;
  if(0==once){
    once = 1;
    builtin_request_js("forum.js");
    style_emit_script_fossil_bootstrap(0);
    builtin_request_js("fossil.dom.js");
    builtin_request_js("fossil.page.forumpost.js");
  }
}

/*
** WEBPAGE: forumpost
**
** Show a single forum posting. The posting is shown in context with
** it's entire thread.  The selected posting is enclosed within
** <div class='forumSel'>...</div>.  Javascript is used to move the
** selected posting into view after the page loads.
**
** Query parameters:
**
**   name=X        REQUIRED.  The hash of the post to display
**   t=MODE        Display mode.
**                   'c' for chronological
**                   'h' for hierarchical
**                   'a' for automatic
**                   'r' for raw
**                   'y' for history of post X only
**   raw           If present, show only the post specified and
**                 show its original unformatted source text.
*/
void forumpost_page(void){
  forumthread_page();
}

/*
** Add an appropriate style_header() to include title of the
** given forum post.
*/
static int forumthread_page_header(int froot, int fpid){
  char *zThreadTitle = 0;

  zThreadTitle = db_text("",
    "SELECT"
    " substr(event.comment,instr(event.comment,':')+2)"
    " FROM forumpost, event"
    " WHERE event.objid=forumpost.fpid"
    "   AND forumpost.fpid=%d;",
    fpid
  );
  style_header("%s%s", zThreadTitle, zThreadTitle[0] ? "" : "Forum");
  fossil_free(zThreadTitle);
  return 0;
}

/*
** WEBPAGE: forumthread
**
** Show all forum messages associated with a particular message thread.
** The result is basically the same as /forumpost except that none of
** the postings in the thread are selected.
**
** Query parameters:
**
**   name=X        REQUIRED.  The hash of any post of the thread.
**   t=MODE        Display mode. MODE is...
**                   'c' for chronological, or
**                   'h' for hierarchical, or
**                   'a' for automatic, or
**                   'r' for raw.
**   raw           Show only the post given by name= and show it unformatted
**   hist          Show only the edit history for the name= post
*/
void forumthread_page(void){
  int fpid;
  int froot;
  const char *zName = P("name");
  const char *zMode = PD("t","a");
  int bRaw = PB("raw");
  login_check_credentials();
  if( !g.perm.RdForum ){
    login_needed(g.anon.RdForum);
    return;
  }
  if( zName==0 ){
    webpage_error("Missing \"name=\" query parameter");
  }
  fpid = symbolic_name_to_rid(zName, "f");
  if( fpid<=0 ){
    webpage_error("Unknown or ambiguous forum id: \"%s\"", zName);
  }
  froot = db_int(0, "SELECT froot FROM forumpost WHERE fpid=%d", fpid);
  if( froot==0 ){
    webpage_error("Not a forum post: \"%s\"", zName);
  }
  if( fossil_strcmp(g.zPath,"forumthread")==0 ) fpid = 0;
  if( zMode[0]=='a' ){
    if( cgi_from_mobile() ){
      zMode = "c";  /* Default to chronological on mobile */
    }else{
      zMode = "h";
    }
  }
  if( zMode[0]!='y' ){
    forumthread_page_header(froot, fpid);
  }
  if( bRaw && fpid ){
    Manifest *pPost;
    pPost = manifest_get(fpid, CFTYPE_FORUM, 0);
    if( pPost==0 ){
      @ <p>No such forum post: %h(zName)
    }else{
      int isPrivate = content_is_private(fpid);
      int notAnon = login_is_individual();
      int sameUser = notAnon && fossil_strcmp(pPost->zUser, g.zLogin)==0;
      if( isPrivate && !g.perm.ModForum && !sameUser ){
        @ <p><span class="modpending">Awaiting Moderator Approval</span></p>
      }else{
        forum_render(0, "text/plain", pPost->zWiki, 0, 0);
      }
      manifest_destroy(pPost);
    }
  }else if( zMode[0]=='c' ){
    style_submenu_element("Hierarchical", "%R/%s/%s?t=h", g.zPath, zName);
    style_submenu_element("Unformatted", "%R/%s/%s?t=r", g.zPath, zName);
    forum_display_chronological(froot, fpid, 0);
  }else if( zMode[0]=='r' ){
    style_submenu_element("Chronological", "%R/%s/%s?t=c", g.zPath, zName);
    style_submenu_element("Hierarchical", "%R/%s/%s?t=h", g.zPath, zName);
    forum_display_chronological(froot, fpid, 1);
  }else if( zMode[0]=='y' ){
    style_header("Edit History Of A Forum Post");
    style_submenu_element("Complete Thread", "%R/%s/%s?t=a", g.zPath, zName);
    forum_display_history(froot, fpid, 1);
  }else{
    style_submenu_element("Chronological", "%R/%s/%s?t=c", g.zPath, zName);
    style_submenu_element("Unformatted", "%R/%s/%s?t=r", g.zPath, zName);
    forum_display_hierarchical(froot, fpid);
  }
  forum_emit_page_js();
  style_footer();
}

/*
** Return true if a forum post should be moderated.
*/
static int forum_need_moderation(void){
  if( P("domod") ) return 1;
  if( g.perm.WrTForum ) return 0;
  if( g.perm.ModForum ) return 0;
  return 1;
}

/*
** Return true if the string is white-space only.
*/
static int whitespace_only(const char *z){
  if( z==0 ) return 1;
  while( z[0] && fossil_isspace(z[0]) ){ z++; }
  return z[0]==0;
}

/*
** Add a new Forum Post artifact to the repository.
**
** Return true if a redirect occurs.
*/
static int forum_post(
  const char *zTitle,          /* Title.  NULL for replies */
  int iInReplyTo,              /* Post replying to.  0 for new threads */
  int iEdit,                   /* Post being edited, or zero for a new post */
  const char *zUser,           /* Username.  NULL means use login name */
  const char *zMimetype,       /* Mimetype of content. */
  const char *zContent         /* Content */
){
  char *zDate;
  char *zI;
  char *zG;
  int iBasis;
  Blob x, cksum, formatCheck, errMsg;
  Manifest *pPost;
  int nContent = zContent ? (int)strlen(zContent) : 0;

  schema_forum();
  if( iEdit==0 && whitespace_only(zContent) ){
    return 0;
  }
  if( iInReplyTo==0 && iEdit>0 ){
    iBasis = iEdit;
    iInReplyTo = db_int(0, "SELECT firt FROM forumpost WHERE fpid=%d", iEdit);
  }else{
    iBasis = iInReplyTo;
  }
  webpage_assert( (zTitle==0)+(iInReplyTo==0)==1 );
  blob_init(&x, 0, 0);
  zDate = date_in_standard_format("now");
  blob_appendf(&x, "D %s\n", zDate);
  fossil_free(zDate);
  zG = db_text(0, 
     "SELECT uuid FROM blob, forumpost"
     " WHERE blob.rid==forumpost.froot"
     "   AND forumpost.fpid=%d", iBasis);
  if( zG ){
    blob_appendf(&x, "G %s\n", zG);
    fossil_free(zG);
  }
  if( zTitle ){
    blob_appendf(&x, "H %F\n", zTitle);
  }
  zI = db_text(0, "SELECT uuid FROM blob WHERE rid=%d", iInReplyTo);
  if( zI ){
    blob_appendf(&x, "I %s\n", zI);
    fossil_free(zI);
  }
  if( fossil_strcmp(zMimetype,"text/x-fossil-wiki")!=0 ){
    blob_appendf(&x, "N %s\n", zMimetype);
  }
  if( iEdit>0 ){
    char *zP = db_text(0, "SELECT uuid FROM blob WHERE rid=%d", iEdit);
    if( zP==0 ) webpage_error("missing edit artifact %d", iEdit);
    blob_appendf(&x, "P %s\n", zP);
    fossil_free(zP);
  }
  if( zUser==0 ){
    if( login_is_nobody() ){
      zUser = "anonymous";
    }else{
      zUser = login_name();
    }
  }
  blob_appendf(&x, "U %F\n", zUser);
  blob_appendf(&x, "W %d\n%s\n", nContent, zContent);
  md5sum_blob(&x, &cksum);
  blob_appendf(&x, "Z %b\n", &cksum);
  blob_reset(&cksum);

  /* Verify that the artifact we are creating is well-formed */
  blob_init(&formatCheck, 0, 0);
  blob_init(&errMsg, 0, 0);
  blob_copy(&formatCheck, &x);
  pPost = manifest_parse(&formatCheck, 0, &errMsg);
  if( pPost==0 ){
    webpage_error("malformed forum post artifact - %s", blob_str(&errMsg));
  }
  webpage_assert( pPost->type==CFTYPE_FORUM );
  manifest_destroy(pPost);

  if( P("dryrun") ){
    @ <div class='debug'>
    @ This is the artifact that would have been generated:
    @ <pre>%h(blob_str(&x))</pre>
    @ </div>
    blob_reset(&x);
    return 0;
  }else{
    int nrid = wiki_put(&x, iEdit>0 ? iEdit : 0,
                        forum_need_moderation());
    blob_reset(&x);
    cgi_redirectf("%R/forumpost/%S", rid_to_uuid(nrid));
    return 1;
  }
}

/*
** Paint the form elements for entering a Forum post
*/
static void forum_entry_widget(
  const char *zTitle,
  const char *zMimetype,
  const char *zContent
){
  if( zTitle ){
    @ Title: <input type="input" name="title" value="%h(zTitle)" size="50"
    @ maxlength="125"><br>
  }
  @ %z(href("%R/markup_help"))Markup style</a>:
  mimetype_option_menu(zMimetype);
  @ <br><textarea aria-label="Content:" name="content" class="wikiedit" \
  @ cols="80" rows="25" wrap="virtual">%h(zContent)</textarea><br>
}

/*
** WEBPAGE: forumnew
** WEBPAGE: forumedit
**
** Start a new thread on the forum or reply to an existing thread.
** But first prompt to see if the user would like to log in.
*/
void forum_page_init(void){
  int isEdit;
  char *zGoto;
  login_check_credentials();
  if( !g.perm.WrForum ){
    login_needed(g.anon.WrForum);
    return;
  }
  if( sqlite3_strglob("*edit*", g.zPath)==0 ){
    zGoto = mprintf("%R/forume2?fpid=%S",PD("fpid",""));
    isEdit = 1;
  }else{
    zGoto = mprintf("%R/forume1");
    isEdit = 0;
  }
  if( login_is_individual() ){
    if( isEdit ){
      forumedit_page();
    }else{
      forumnew_page();
    }
    return;
  }
  style_header("%h As Anonymous?", isEdit ? "Reply" : "Post");
  @ <p>You are not logged in.
  @ <p><table border="0" cellpadding="10">
  @ <tr><td>
  @ <form action="%s(zGoto)" method="POST">
  @ <input type="submit" value="Remain Anonymous">
  @ </form>
  @ <td>Post to the forum anonymously
  if( login_self_register_available(0) ){
    @ <tr><td>
    @ <form action="%R/register" method="POST">
    @ <input type="hidden" name="g" value="%s(zGoto)">
    @ <input type="submit" value="Create An Account">
    @ </form>
    @ <td>Create a new account and post using that new account
  }
  @ <tr><td>
  @ <form action="%R/login" method="POST">
  @ <input type="hidden" name="g" value="%s(zGoto)">
  @ <input type="hidden" name="noanon" value="1">
  @ <input type="submit" value="Login">
  @ </form>
  @ <td>Log into an existing account
  @ </table>
  style_footer();
  fossil_free(zGoto);
}

/*
** Write the "From: USER" line on the webpage.
*/
static void forum_from_line(void){
  if( login_is_nobody() ){
    @ From: anonymous<br>
  }else{
    @ From: %h(login_name())<br>
  }
}

/*
** WEBPAGE: forume1
**
** Start a new forum thread.
*/
void forumnew_page(void){
  const char *zTitle = PDT("title","");
  const char *zMimetype = PD("mimetype",DEFAULT_FORUM_MIMETYPE);
  const char *zContent = PDT("content","");
  login_check_credentials();
  if( !g.perm.WrForum ){
    login_needed(g.anon.WrForum);
    return;
  }
  if( P("submit") && cgi_csrf_safe(1) ){
    if( forum_post(zTitle, 0, 0, 0, zMimetype, zContent) ) return;
  }
  if( P("preview") && !whitespace_only(zContent) ){
    @ <h1>Preview:</h1>
    forum_render(zTitle, zMimetype, zContent, "forumEdit", 1);
  }
  style_header("New Forum Thread");
  @ <form action="%R/forume1" method="POST">
  @ <h1>New Thread:</h1>
  forum_from_line();
  forum_entry_widget(zTitle, zMimetype, zContent);
  @ <input type="submit" name="preview" value="Preview">
  if( P("preview") && !whitespace_only(zContent) ){
    @ <input type="submit" name="submit" value="Submit">
  }else{
    @ <input type="submit" name="submit" value="Submit" disabled>
  }
  if( g.perm.Debug ){
    /* Give extra control over the post to users with the special
     * Debug capability, which includes Admin and Setup users */
    @ <div class="debug">
    @ <label><input type="checkbox" name="dryrun" %s(PCK("dryrun"))> \
    @ Dry run</label>
    @ <br><label><input type="checkbox" name="domod" %s(PCK("domod"))> \
    @ Require moderator approval</label>
    @ <br><label><input type="checkbox" name="showqp" %s(PCK("showqp"))> \
    @ Show query parameters</label>
    @ </div>
  }
  @ </form>
  style_footer();
}

/*
** WEBPAGE: forume2
**
** Edit an existing forum message.
** Query parameters:
**
**   fpid=X        Hash of the post to be editted.  REQUIRED
*/
void forumedit_page(void){
  int fpid;
  int froot;
  Manifest *pPost = 0;
  Manifest *pRootPost = 0;
  const char *zMimetype = 0;
  const char *zContent = 0;
  const char *zTitle = 0;
  char *zDate = 0;
  int isCsrfSafe;
  int isDelete = 0;

  login_check_credentials();
  if( !g.perm.WrForum ){
    login_needed(g.anon.WrForum);
    return;
  }
  fpid = symbolic_name_to_rid(PD("fpid",""), "f");
  if( fpid<=0 || (pPost = manifest_get(fpid, CFTYPE_FORUM, 0))==0 ){
    webpage_error("Missing or invalid fpid query parameter");
  }
  froot = db_int(0, "SELECT froot FROM forumpost WHERE fpid=%d", fpid);
  if( froot==0 || (pRootPost = manifest_get(froot, CFTYPE_FORUM, 0))==0 ){
    webpage_error("fpid does not appear to be a forum post: \"%d\"", fpid);
  }
  if( P("cancel") ){
    cgi_redirectf("%R/forumpost/%S",P("fpid"));
    return;
  }
  isCsrfSafe = cgi_csrf_safe(1);
  if( g.perm.ModForum && isCsrfSafe ){
    if( P("approve") ){
      const char *zUserToTrust;
      moderation_approve('f', fpid);
      if( g.perm.AdminForum
       && PB("trust")
       && (zUserToTrust = P("trustuser"))!=0
      ){
        db_multi_exec("UPDATE user SET cap=cap||'4' "
                      "WHERE login=%Q AND cap NOT GLOB '*4*'",
                      zUserToTrust);
      }
      cgi_redirectf("%R/forumpost/%S",P("fpid"));
      return;
    }
    if( P("reject") ){
      char *zParent = 
        db_text(0,
          "SELECT uuid FROM forumpost, blob"
          " WHERE forumpost.fpid=%d AND blob.rid=forumpost.firt",
          fpid
        );
      moderation_disapprove(fpid);
      if( zParent ){
        cgi_redirectf("%R/forumpost/%S",zParent);
      }else{
        cgi_redirectf("%R/forum");
      }
      return;
    }
  }
  isDelete = P("nullout")!=0;
  if( P("submit")
   && isCsrfSafe
   && (zContent = PDT("content",""))!=0
   && (!whitespace_only(zContent) || isDelete)
  ){
    int done = 1;
    const char *zMimetype = PD("mimetype",DEFAULT_FORUM_MIMETYPE);
    if( P("reply") ){
      done = forum_post(0, fpid, 0, 0, zMimetype, zContent);
    }else if( P("edit") || isDelete ){
      done = forum_post(P("title"), 0, fpid, 0, zMimetype, zContent);
    }else{
      webpage_error("Missing 'reply' query parameter");
    }
    if( done ) return;
  }
  if( isDelete ){
    zMimetype = "text/x-fossil-wiki";
    zContent = "";
    if( pPost->zThreadTitle ) zTitle = "";
    style_header("Delete %s", zTitle ? "Post" : "Reply");
    @ <h1>Original Post:</h1>
    forum_render(pPost->zThreadTitle, pPost->zMimetype, pPost->zWiki,
                 "forumEdit", 1);
    @ <h1>Change Into:</h1>
    forum_render(zTitle, zMimetype, zContent,"forumEdit", 1);
    @ <form action="%R/forume2" method="POST">
    @ <input type="hidden" name="fpid" value="%h(P("fpid"))">
    @ <input type="hidden" name="nullout" value="1">
    @ <input type="hidden" name="mimetype" value="%h(zMimetype)">
    @ <input type="hidden" name="content" value="%h(zContent)">
    if( zTitle ){
      @ <input aria-label="Title" type="hidden" name="title" value="%h(zTitle)">
    }
  }else if( P("edit") ){
    /* Provide an edit to the fpid post */
    zMimetype = P("mimetype");
    zContent = PT("content");
    zTitle = P("title");
    if( zContent==0 ) zContent = fossil_strdup(pPost->zWiki);
    if( zMimetype==0 ) zMimetype = fossil_strdup(pPost->zMimetype);
    if( zTitle==0 && pPost->zThreadTitle!=0 ){
      zTitle = fossil_strdup(pPost->zThreadTitle);
    }
    style_header("Edit %s", zTitle ? "Post" : "Reply");
    @ <h2>Original Post:</h2>
    forum_render(pPost->zThreadTitle, pPost->zMimetype, pPost->zWiki,
                 "forumEdit", 1);
    if( P("preview") ){
      @ <h2>Preview of Edited Post:</h2>
      forum_render(zTitle, zMimetype, zContent,"forumEdit", 1);
    }
    @ <h2>Revised Message:</h2>
    @ <form action="%R/forume2" method="POST">
    @ <input type="hidden" name="fpid" value="%h(P("fpid"))">
    @ <input type="hidden" name="edit" value="1">
    forum_from_line();
    forum_entry_widget(zTitle, zMimetype, zContent);
  }else{
    /* Reply */
    char *zDisplayName;
    zMimetype = PD("mimetype",DEFAULT_FORUM_MIMETYPE);
    zContent = PDT("content","");
    style_header("Reply");
    if( pRootPost->zThreadTitle ){
      @ <h1>Thread: %h(pRootPost->zThreadTitle)</h1>
    }
    @ <h2>Replying To:</h2>
    zDate = db_text(0, "SELECT datetime(%.17g)", pPost->rDate);
    zDisplayName = display_name_from_login(pPost->zUser);
    @ <h3 class='forumPostHdr'>By %h(zDisplayName) on %h(zDate)</h3>
    fossil_free(zDisplayName);
    fossil_free(zDate);
    forum_render(0, pPost->zMimetype, pPost->zWiki, "forumEdit", 1);
    if( P("preview") && !whitespace_only(zContent) ){
      @ <h2>Preview:</h2>
      forum_render(0, zMimetype,zContent, "forumEdit", 1);
    }
    @ <h2>Enter Reply:</h2>
    @ <form action="%R/forume2" method="POST">
    @ <input type="hidden" name="fpid" value="%h(P("fpid"))">
    @ <input type="hidden" name="reply" value="1">
    forum_from_line();
    forum_entry_widget(0, zMimetype, zContent);
  }
  if( !isDelete ){
    @ <input type="submit" name="preview" value="Preview">
  }
  @ <input type="submit" name="cancel" value="Cancel">
  if( (P("preview") && !whitespace_only(zContent)) || isDelete ){
    @ <input type="submit" name="submit" value="Submit">
  }
  if( g.perm.Debug ){
    /* For the test-forumnew page add these extra debugging controls */
    @ <div class="debug">
    @ <label><input type="checkbox" name="dryrun" %s(PCK("dryrun"))> \
    @ Dry run</label>
    @ <br><label><input type="checkbox" name="domod" %s(PCK("domod"))> \
    @ Require moderator approval</label>
    @ <br><label><input type="checkbox" name="showqp" %s(PCK("showqp"))> \
    @ Show query parameters</label>
    @ </div>
  }
  @ </form>
  style_footer();
}

/*
** WEBPAGE: forummain
** WEBPAGE: forum
**
** The main page for the forum feature.  Show a list of recent forum
** threads.  Also show a search box at the top if search is enabled,
** and a button for creating a new thread, if enabled.
**
** Query parameters:
**
**    n=N             The number of threads to show on each page
**    x=X             Skip the first X threads
*/
void forum_main_page(void){
  Stmt q;
  int iLimit, iOfst, iCnt;
  int srchFlags;
  login_check_credentials();
  srchFlags = search_restrict(SRCH_FORUM);
  if( !g.perm.RdForum ){
    login_needed(g.anon.RdForum);
    return;
  }
  style_header("Forum");
  if( g.perm.WrForum ){
    style_submenu_element("New Thread","%R/forumnew");
  }else{
    /* Can't combine this with previous case using the ternary operator
     * because that causes an error yelling about "non-constant format"
     * with some compilers.  I can't see it, since both expressions have
     * the same format, but I'm no C spec lawyer. */
    style_submenu_element("New Thread","%R/login");
  }
  if( g.perm.ModForum && moderation_needed() ){
    style_submenu_element("Moderation Requests", "%R/modreq");
  }
  if( (srchFlags & SRCH_FORUM)!=0 ){
    if( search_screen(SRCH_FORUM, 0) ){
      style_submenu_element("Recent Threads","%R/forum");
      style_footer();
      return;
    }
  }
  iLimit = atoi(PD("n","25"));
  iOfst = atoi(PD("x","0"));
  iCnt = 0;
  if( db_table_exists("repository","forumpost") ){
    db_prepare(&q,
      "WITH thread(age,duration,cnt,root,last) AS ("
      "  SELECT"
      "    julianday('now') - max(fmtime),"
      "    max(fmtime) - min(fmtime),"
      "    sum(fprev IS NULL),"
      "    froot,"
      "    (SELECT fpid FROM forumpost AS y"
      "      WHERE y.froot=x.froot %s"
      "      ORDER BY y.fmtime DESC LIMIT 1)"
      "  FROM forumpost AS x"
      "  WHERE %s"
      "  GROUP BY froot"
      "  ORDER BY 1 LIMIT %d OFFSET %d"
      ")"
      "SELECT"
      "  thread.age,"                                         /* 0 */
      "  thread.duration,"                                    /* 1 */
      "  thread.cnt,"                                         /* 2 */
      "  blob.uuid,"                                          /* 3 */
      "  substr(event.comment,instr(event.comment,':')+1),"   /* 4 */
      "  thread.last"                                         /* 5 */
      " FROM thread, blob, event"
      " WHERE blob.rid=thread.last"
      "  AND event.objid=thread.last"
      " ORDER BY 1;",
      g.perm.ModForum ? "" : "AND y.fpid NOT IN private" /*safe-for-%s*/,
      g.perm.ModForum ? "true" : "fpid NOT IN private" /*safe-for-%s*/,
      iLimit+1, iOfst
    );
    while( db_step(&q)==SQLITE_ROW ){
      char *zAge = human_readable_age(db_column_double(&q,0));
      int nMsg = db_column_int(&q, 2);
      const char *zUuid = db_column_text(&q, 3);
      const char *zTitle = db_column_text(&q, 4);
      if( iCnt==0 ){
        if( iOfst>0 ){
          @ <h1>Threads at least %s(zAge) old</h1>
        }else{
          @ <h1>Most recent threads</h1>
        }
        @ <div class='forumPosts fileage'><table width="100%%">
        if( iOfst>0 ){
          if( iOfst>iLimit ){
            @ <tr><td colspan="3">\
            @ %z(href("%R/forum?x=%d&n=%d",iOfst-iLimit,iLimit))\
            @ &uarr; Newer...</a></td></tr>
          }else{
            @ <tr><td colspan="3">%z(href("%R/forum?n=%d",iLimit))\
            @ &uarr; Newer...</a></td></tr>
          }
        }
      }
      iCnt++;
      if( iCnt>iLimit ){
        @ <tr><td colspan="3">\
        @ %z(href("%R/forum?x=%d&n=%d",iOfst+iLimit,iLimit))\
        @ &darr; Older...</a></td></tr>
        fossil_free(zAge);
        break;
      }
      @ <tr><td>%h(zAge) ago</td>
      @ <td>%z(href("%R/forumpost/%S",zUuid))%h(zTitle)</a></td>
      @ <td>\
      if( g.perm.ModForum && moderation_pending(db_column_int(&q,5)) ){
        @ <span class="modpending">\
        @ Awaiting Moderator Approval</span><br>
      }
      if( nMsg<2 ){
        @ no replies</td>
      }else{
        char *zDuration = human_readable_age(db_column_double(&q,1));
        @ %d(nMsg) posts spanning %h(zDuration)</td>
        fossil_free(zDuration);
      }
      @ </tr>
      fossil_free(zAge);
    }
    db_finalize(&q);
  }
  if( iCnt>0 ){
    @ </table></div>
  }else{
    @ <h1>No forum posts found</h1>
  }
  style_footer();
}
