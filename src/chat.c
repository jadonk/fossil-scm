/*
** Copyright (c) 2020 D. Richard Hipp
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
** This file contains code used to implement the Fossil chatroom.
**
** Initial design goals:
**
**    *   Keep it simple.  This chatroom is not intended as a competitor
**        or replacement for IRC, Discord, Telegram, Slack, etc.  The goal
**        is zero- or near-zero-configuration, not an abundance of features.
**
**    *   Intended as a place for insiders to have ephemeral conversations
**        about a project.  This is not a public gather place.  Think
**        "boardroom", not "corner pub".
**
**    *   One chatroom per repository.
**
**    *   Chat content lives in a single repository.  It is never synced.
**        Content expires and is deleted after a set interval (a week or so).
**
** Notification is accomplished using the "hanging GET" or "long poll" design
** in which a GET request is issued but the server does not send a reply until
** new content arrives.  Newer Web Sockets and Server Sent Event protocols are
** more elegant, but are not compatible with CGI, and would thus complicate
** configuration.  
*/
#include "config.h"
#include <assert.h>
#include "chat.h"

/* Settings that can be used to control chat */
/*
** SETTING: chat-initial-history    width=10 default=50
**
** If this setting has an integer value of N, then when /chat first
** starts up it initializes the screen with the N most recent chat
** messages.  If N is zero, then all chat messages are loaded.
*/
/*
** SETTING: chat-keep-count    width=10 default=50
**
** When /chat is cleaning up older messages, it will always keep
** the most recent chat-keep-count messages, even if some of those
** messages are older than the discard threshold.  If this value
** is zero, then /chat is free to delete all historic messages once
** they are old enough.
*/
/*
** SETTING: chat-keep-days    width=10 default=7
**
** The /chat subsystem will try to discard messages that are older then
** chat-keep-days.  The value of chat-keep-days can be a floating point
** number.  So, for example, if you only want to keep chat messages for
** 12 hours, set this value to 0.5.
**
** A value of 0.0 or less means that messages are retained forever.
*/
/*
** SETTING: chat-inline-images    boolean default=on
**
** Specifies whether posted images in /chat should default to being
** displayed inline or as downloadable links. Each chat user can
** change this value for their current chat session in the UI.
*/
/*
** SETTING: chat-poll-timeout    width=10 default=420
**
** On an HTTP request to /chat-poll, if there is no new content available,
** the reply is delayed waiting for new content to arrive.  (This is the
** "long poll" strategy of event delivery to the client.)  This setting
** determines approximately how long /chat-poll will delay before giving
** up and returning an empty reply.  The default value is about 7 minutes,
** which works well for Fossil behind the althttpd web server.  Other
** server environments may choose a longer or shorter delay.
**
** For maximum efficiency, it is best to choose the longest delay that
** does not cause timeouts in intermediate proxies or web server.
*/
/*
** WEBPAGE: chat
**
** Start up a browser-based chat session.
**
** This is the main page that humans use to access the chatroom.  Simply
** point a web-browser at /chat and the screen fills with the latest
** chat messages, and waits for new one.
**
** Other /chat-OP pages are used by XHR requests from this page to
** send new chat message, delete older messages, or poll for changes.
*/
void chat_webpage(void){
  int iPingTcp;
  login_check_credentials();
  if( !g.perm.Chat ){
    login_needed(g.anon.Chat);
    return;
  }
  iPingTcp = atoi(PD("ping","0"));
  if( iPingTcp<1000 || iPingTcp>65535 ) iPingTcp = 0;
  if( iPingTcp ) style_disable_csp();
  style_set_current_feature("chat");
  style_header("Chat");
  @ <form accept-encoding="utf-8" id="chat-form" autocomplete="off">
  @ <div id='chat-input-area'>
  @   <div id='chat-input-line'>
  @     <input type="text" name="msg" id="chat-input-single" \
  @      placeholder="Type message here." autocomplete="off">
  @     <textarea rows="8" id="chat-input-multi" \
  @      placeholder="Type message here. Ctrl-Enter sends it." \
  @      class="hidden"></textarea>
  @     <input type="submit" value="Send" id="chat-message-submit">
  @     <button id="chat-scroll-top">&uarr;</button>
  @     <button id="chat-scroll-bottom">&darr;</button>
  @     <span id="chat-settings-button" class="settings-icon" \
  @       aria-label="Settings..." aria-haspopup="true" ></span>
  @   </div>
  @   <div id='chat-input-file-area'>
  @     <div class='file-selection-wrapper'>
  @       <div class='help-buttonlet'>
  @        Select a file to upload, drag/drop a file into this spot,
  @        or paste an image from the clipboard if supported by
  @        your environment.
  @       </div>
  @       <input type="file" name="file" id="chat-input-file">
  @     </div>
  @     <div id="chat-drop-details"></div>
  @   </div>
  @ </div>
  @ </form>
  @ <div id='chat-messages-wrapper'>
  /* New chat messages get inserted immediately after this element */
  @ <span id='message-inject-point'></span>
  @ </div>

  builtin_fossil_js_bundle_or("popupwidget", "storage", NULL);
  /* Always in-line the javascript for the chat page */
  @ <script nonce="%h(style_nonce())">/* chat.c:%d(__LINE__) */
  /* We need an onload handler to ensure that window.fossil is
     initialized before the chat init code runs. */
  @ window.addEventListener('load', function(){
  @ document.body.classList.add('chat')
  @ /*^^^for skins which add their own BODY tag */;
  @ window.fossil.config.chat = {
  @   pingTcp: %d(iPingTcp),
  @   initSize: %d(db_get_int("chat-initial-history",50)),
  @   imagesInline: !!%d(db_get_boolean("chat-inline-images",1))
  @ };
  cgi_append_content(builtin_text("chat.js"),-1);
  @ }, false);
  @ </script>

  style_finish_page();
}

/* Definition of repository tables used by chat
*/
static const char zChatSchema1[] =
@ CREATE TABLE repository.chat(
@   msgid INTEGER PRIMARY KEY AUTOINCREMENT,
@   mtime JULIANDAY,       -- Time for this entry - Julianday Zulu
@   lmtime TEXT,           -- Localtime when message originally sent
@   xfrom TEXT,            -- Login of the sender
@   xmsg  TEXT,            -- Raw, unformatted text of the message
@   fname TEXT,            -- Filename of the uploaded file, or NULL
@   fmime TEXT,            -- MIMEType of the upload file, or NULL
@   mdel INT,              -- msgid of another message to delete
@   file  BLOB             -- Text of the uploaded file, or NULL
@ );
;


/*
** Make sure the repository data tables used by chat exist.  Create them
** if they do not.
*/
static void chat_create_tables(void){
  if( !db_table_exists("repository","chat") ){
    db_multi_exec(zChatSchema1/*works-like:""*/);
  }else if( !db_table_has_column("repository","chat","lmtime") ){
    if( !db_table_has_column("repository","chat","mdel") ){
      db_multi_exec("ALTER TABLE chat ADD COLUMN mdel INT");
    }
    db_multi_exec("ALTER TABLE chat ADD COLUMN lmtime TEXT");
  }
}

/*
** Delete old content from the chat table.
*/
static void chat_purge(void){
   int mxCnt = db_get_int("chat-keep-count",50);
   double mxDays = atof(db_get("chat-keep-days","7"));
   double rAge;
   int msgid;
   rAge = db_double(0.0, "SELECT julianday('now')-mtime FROM chat"
                         " ORDER BY msgid LIMIT 1");
   if( rAge>mxDays ){
     msgid = db_int(0, "SELECT msgid FROM chat"
                       " ORDER BY msgid DESC LIMIT 1 OFFSET %d", mxCnt);
     if( msgid>0 ){
       Stmt s;
       db_prepare(&s, 
             "DELETE FROM chat WHERE mtime<julianday('now')-:mxage"
             " AND msgid<%d", msgid);
       db_bind_double(&s, ":mxage", mxDays);
       db_step(&s);
       db_finalize(&s);
     }
   }
}

/*
** Sets the current CGI response type to application/json then emits a
** JSON-format error message object. If fAsMessageList is true then
** the object is output using the list format described for chat-poll,
** else it is emitted as a single object in that same format.
*/
static void chat_emit_permissions_error(int fAsMessageList){
  char * zTime = cgi_iso8601_datestamp();
  cgi_set_content_type("application/json");
  if(fAsMessageList){
    CX("{\"msgs\":[{");
  }else{
    CX("{");
  }
  CX("\"isError\": true, \"xfrom\": \"fossil\",");
  CX("\"mtime\": %!j, \"lmtime\": %!j,", zTime, zTime);
  CX("\"xmsg\": \"Missing permissions or not logged in. "
     "Try <a href='%R/login?g=%R/chat'>logging in</a>.\"");
  if(fAsMessageList){
    CX("}]}");
  }else{
    CX("}");
  }
  fossil_free(zTime);
}

/*
** WEBPAGE: chat-send
**
** This page receives (via XHR) a new chat-message and/or a new file
** to be entered into the chat history.
**
** On success it responds with an empty response: the new message
** should be fetched via /chat-poll. On error, e.g. login expiry,
** it emits a JSON response in the same form as described for
** /chat-poll errors, but as a standalone object instead of a
** list of objects.
*/
void chat_send_webpage(void){
  int nByte;
  const char *zMsg;
  login_check_credentials();
  if( !g.perm.Chat ) {
    chat_emit_permissions_error(0);
    return;
  }
  chat_create_tables();
  nByte = atoi(PD("file:bytes",0));
  zMsg = PD("msg","");
  db_begin_write();
  chat_purge();
  if( nByte==0 ){
    if( zMsg[0] ){
      db_multi_exec(
        "INSERT INTO chat(mtime,lmtime,xfrom,xmsg)"
        "VALUES(julianday('now'),%Q,%Q,%Q)",
        P("lmtime"), g.zLogin, zMsg
      );
    }
  }else{
    Stmt q;
    Blob b;
    db_prepare(&q,
        "INSERT INTO chat(mtime,lmtime,xfrom,xmsg,file,fname,fmime)"
        "VALUES(julianday('now'),%Q,%Q,%Q,:file,%Q,%Q)",
        P("lmtime"), g.zLogin, zMsg, PD("file:filename",""),
        PD("file:mimetype","application/octet-stream"));
    blob_init(&b, P("file"), nByte);
    db_bind_blob(&q, ":file", &b);
    db_step(&q);
    db_finalize(&q);
    blob_reset(&b);
  }
  db_commit_transaction();
}

/*
** This routine receives raw (user-entered) message text and transforms
** it into HTML that is safe to insert using innerHTML.
**
**    *   HTML in the original text is escaped.
**
**    *   Hyperlinks are identified and tagged.  Hyperlinks are:
**
**          -  Undelimited text of the form https:... or http:...
**          -  Any text enclosed within [...]
**
** Space to hold the returned string is obtained from fossil_malloc()
** and must be freed by the caller.
*/
static char *chat_format_to_html(const char *zMsg){
  char *zSafe = mprintf("%h", zMsg);
  int i, j, k;
  Blob out;
  char zClose[20];
  blob_init(&out, 0, 0);
  for(i=j=0; zSafe[i]; i++){
    if( zSafe[i]=='[' ){
      for(k=i+1; zSafe[k] && zSafe[k]!=']'; k++){}
      if( zSafe[k]==']' ){
        zSafe[k] = 0;
        if( j<i ){
          blob_append(&out, zSafe + j, i-j);
          j = i;
        }
        wiki_resolve_hyperlink(&out, WIKI_NOBADLINKS|WIKI_TARGET_BLANK,
                               zSafe+i+1, zClose, sizeof(zClose), zSafe, 0);
        zSafe[k] = ']';
        j++;
        blob_append(&out, zSafe + j, k - j);
        blob_append(&out, zClose, -1);
        i = k;
        j = k+1;
        continue;
      }
    }else if( zSafe[i]=='h' 
           && (strncmp(zSafe+i,"http:",5)==0
               || strncmp(zSafe+i,"https:",6)==0) ){
      for(k=i+1; zSafe[k] && !fossil_isspace(zSafe[k]); k++){}
      if( k>i+7 ){
        char c = zSafe[k];
        if( !fossil_isalnum(zSafe[k-1]) && zSafe[k-1]!='/' ){
          k--;
          c = zSafe[k];
        }
        if( j<i ){
          blob_append(&out, zSafe + j, i-j);
          j = i;
        }
        zSafe[k] = 0;        
        wiki_resolve_hyperlink(&out, WIKI_NOBADLINKS|WIKI_TARGET_BLANK,
                               zSafe+i, zClose, sizeof(zClose), zSafe, 0);
        zSafe[k] = c;
        blob_append(&out, zSafe + j, k - j);
        blob_append(&out, zClose, -1);
        i = j = k;
        continue;
      }
    }
  }
  if( j<i ){
    blob_append(&out, zSafe+j, j-i);
  }
  fossil_free(zSafe);
  return blob_str(&out);
}

/*
** COMMAND: test-chat-formatter
**
** Usage: %fossil test-chat-formatter STRING ...
**
** Transform each argument string into HTML that will display the
** chat message.  This is used to test the formatter and to verify
** that a malicious message text will not cause HTML or JS injection
** into the chat display in a browser.
*/
void chat_test_formatter_cmd(void){
  int i;
  char *zOut;
  db_find_and_open_repository(0,0);
  g.perm.Hyperlink = 1;
  for(i=0; i<g.argc; i++){
    zOut = chat_format_to_html(g.argv[i]);
    fossil_print("[%d]: %s\n", i, zOut);
    fossil_free(zOut);
  }
}

/*
** WEBPAGE: chat-poll
**
** The chat page generated by /chat using an XHR to this page to
** request new chat content.  A typical invocation is:
**
**     /chat-poll/N
**     /chat-poll?name=N
**
** The "name" argument should begin with an integer which is the largest
** "msgid" that the chat page currently holds. If newer content is
** available, this routine returns that content straight away. If no new
** content is available, this webpage blocks until the new content becomes
** available.  In this way, the system implements "hanging-GET" or "long-poll"
** style event notification. If no new content arrives after a delay of
** approximately chat-poll-timeout seconds (default: 420), then reply is
** sent with an empty "msg": field.
**
** If N is negative, then the return value is the N most recent messages.
** Hence a request like /chat-poll/-100 can be used to initialize a new
** chat session to just the most recent messages.
**
** Some webservers (althttpd) do not allow a term of the URL path to
** begin with "-".  Then /chat-poll/-100 cannot be used.  Instead you
** have to say "/chat-poll?name=-100".
**
** If the integer parameter "before" is passed in, it is assumed that
** the client is requesting older messages, up to (but not including)
** that message ID, in which case the next-oldest "n" messages
** (default=chat-initial-history setting, equivalent to n=0) are
** returned (negative n fetches all older entries). The client then
** needs to take care to inject them at the end of the history rather
** than the same place new messages go.
**
** If "before" is provided, "name" is ignored.
**
** The reply from this webpage is JSON that describes the new content.
** Format of the json:
**
** |    {
** |      "msgs":[
** |        {
** |           "msgid": integer // message id
** |           "mtime": text    // When sent:  YYYY-MM-DD HH:MM:SS UTC
** |           "lmtime: text    // Localtime where the message was sent from
** |           "xfrom": text    // Login name of sender
** |           "uclr":  text    // Color string associated with the user
** |           "xmsg":  text    // HTML text of the message
** |           "fsize": integer // file attachment size in bytes
** |           "fname": text    // Name of file attachment
** |           "fmime": text    // MIME-type of file attachment
** |           "mdel":  integer // message id of prior message to delete
** |        }
** |      ]
** |    }
**
** The "fname" and "fmime" fields are only present if "fsize" is greater
** than zero.  The "xmsg" field may be an empty string if "fsize" is zero.
**
** The "msgid" values will be in increasing order.
**
** The "mdel" will only exist if "xmsg" is an empty string and "fsize" is zero.
**
** The "lmtime" value might be known, in which case it is omitted.
**
** The messages are ordered oldest first unless "before" is provided, in which
** case they are sorted newest first (to facilitate the client-side UI update).
**
** As a special case, if this routine encounters an error, e.g. the user's
** permissions cannot be verified because their login cookie expired, the
** request returns a slightly modified structure:
**
** |    {
** |      "msgs":[
** |        {
** |          "isError": true,
** |          "xfrom": "fossil",
** |          "xmsg": "error details"
** |          "mtime": as above,
** |          "ltime": same as mtime
** |        }
** |      ]
** |    }
**
** If the client gets such a response, it should display the message
** in a prominent manner and then stop polling for new messages.
*/
void chat_poll_webpage(void){
  Blob json;                  /* The json to be constructed and returned */
  sqlite3_int64 dataVersion;  /* Data version.  Used for polling. */
  const int iDelay = 1000;    /* Delay until next poll (milliseconds) */
  int nDelay;                 /* Maximum delay.*/
  int msgid = atoi(PD("name","0"));
  const int msgBefore = atoi(PD("before","0"));
  int nLimit = msgBefore>0 ? atoi(PD("n","0")) : 0;
  Blob sql = empty_blob;
  Stmt q1;
  nDelay = db_get_int("chat-poll-timeout",420);  /* Default about 7 minutes */
  login_check_credentials();
  if( !g.perm.Chat ) {
    chat_emit_permissions_error(1);
    return;
  }
  chat_create_tables();
  cgi_set_content_type("application/json");
  dataVersion = db_int64(0, "PRAGMA data_version");
  blob_append_sql(&sql,
    "SELECT msgid, datetime(mtime), xfrom, xmsg, length(file),"
    "       fname, fmime, %s, lmtime"
    "  FROM chat ",
    msgBefore>0 ? "0 as mdel" : "mdel");
  if( msgid<=0 || msgBefore>0 ){
    db_begin_write();
    chat_purge();
    db_commit_transaction();
  }
  if(msgBefore>0){
    if(0==nLimit){
      nLimit = db_get_int("chat-initial-history",50);
    }
    blob_append_sql(&sql,
      " WHERE msgid<%d"
      " ORDER BY msgid DESC "
      "LIMIT %d",
      msgBefore, nLimit>0 ? nLimit : -1
    );
  }else{
    if( msgid<0 ){
      msgid = db_int(0,
            "SELECT msgid FROM chat WHERE mdel IS NOT true"
            " ORDER BY msgid DESC LIMIT 1 OFFSET %d", -msgid);
    }
    blob_append_sql(&sql,
      " WHERE msgid>%d"
      " ORDER BY msgid",
      msgid
    );
  }
  db_prepare(&q1, "%s", blob_sql_text(&sql));
  blob_reset(&sql);
  blob_init(&json, "{\"msgs\":[\n", -1);
  while( nDelay>0 ){
    int cnt = 0;
    while( db_step(&q1)==SQLITE_ROW ){
      int id = db_column_int(&q1, 0);
      const char *zDate = db_column_text(&q1, 1);
      const char *zFrom = db_column_text(&q1, 2);
      const char *zRawMsg = db_column_text(&q1, 3);
      int nByte = db_column_int(&q1, 4);
      const char *zFName = db_column_text(&q1, 5);
      const char *zFMime = db_column_text(&q1, 6);
      int iToDel = db_column_int(&q1, 7);
      const char *zLMtime = db_column_text(&q1, 8);
      char *zMsg;
      if(cnt++){
        blob_append(&json, ",\n", 2);
      }
      blob_appendf(&json, "{\"msgid\":%d,", id);
      blob_appendf(&json, "\"mtime\":\"%.10sT%sZ\",", zDate, zDate+11);
      if( zLMtime && zLMtime[0] ){
        blob_appendf(&json, "\"lmtime\":%!j,", zLMtime);
      }
      blob_appendf(&json, "\"xfrom\":%!j,", zFrom);
      blob_appendf(&json, "\"uclr\":%!j,", hash_color(zFrom));

      zMsg = chat_format_to_html(zRawMsg ? zRawMsg : "");
      blob_appendf(&json, "\"xmsg\":%!j,", zMsg);
      fossil_free(zMsg);

      if( nByte==0 ){
        blob_appendf(&json, "\"fsize\":0");
      }else{
        blob_appendf(&json, "\"fsize\":%d,\"fname\":%!j,\"fmime\":%!j",
               nByte, zFName, zFMime);
      }

      if( iToDel ){
        blob_appendf(&json, ",\"mdel\":%d}", iToDel);
      }else{
        blob_append(&json, "}", 1);
      }
    }
    db_reset(&q1);
    if( cnt || msgBefore>0 ){
      break;
    }
    sqlite3_sleep(iDelay); nDelay--;
    while( nDelay>0 ){
      sqlite3_int64 newDataVers = db_int64(0,"PRAGMA repository.data_version");
      if( newDataVers!=dataVersion ){
        dataVersion = newDataVers;
        break;
      }
      sqlite3_sleep(iDelay); nDelay--;
    }
  } /* Exit by "break" */
  db_finalize(&q1);
  blob_append(&json, "\n]}", 3);
  cgi_set_content(&json);
  return;      
}

/*
** WEBPAGE: chat-download
**
** Download the CHAT.FILE attachment associated with a single chat
** entry.  The "name" query parameter begins with an integer that
** identifies the particular chat message. The integer may be followed
** by a / and a filename, which will indicate to the browser to use
** the indicated name when saving the file.
*/
void chat_download_webpage(void){
  int msgid;
  Blob r;
  const char *zMime;
  login_check_credentials();
  if( !g.perm.Chat ){
    style_header("Chat Not Authorized");
    @ <h1>Not Authorized</h1>
    @ <p>You do not have permission to use the chatroom on this
    @ repository.</p>
    style_finish_page();
    return;
  }
  chat_create_tables();
  msgid = atoi(PD("name","0"));
  blob_zero(&r);
  zMime = db_text(0, "SELECT fmime FROM chat wHERE msgid=%d", msgid);
  if( zMime==0 ) return;
  db_blob(&r, "SELECT file FROM chat WHERE msgid=%d", msgid);
  cgi_set_content_type(zMime);
  cgi_set_content(&r);
}


/*
** WEBPAGE: chat-delete
**
** Delete the chat entry identified by the name query parameter.
** Invoking fetch("chat-delete/"+msgid) from javascript in the client
** will delete a chat entry from the CHAT table.
**
** This routine both deletes the identified chat entry and also inserts
** a new entry with the current timestamp and with:
**
**   *  xmsg = NULL
**   *  file = NULL
**   *  mdel = The msgid of the row that was deleted
**
** This new entry will then be propagated to all listeners so that they
** will know to delete their copies of the message too.
*/
void chat_delete_webpage(void){
  int mdel;
  char *zOwner;
  login_check_credentials();
  if( !g.perm.Chat ) return;
  chat_create_tables();
  mdel = atoi(PD("name","0"));
  zOwner = db_text(0, "SELECT xfrom FROM chat WHERE msgid=%d", mdel);
  if( zOwner==0 ) return;
  if( fossil_strcmp(zOwner, g.zLogin)!=0 && !g.perm.Admin ) return;
  db_multi_exec(
    "BEGIN;\n"
    "DELETE FROM chat WHERE msgid=%d;\n"
    "INSERT INTO chat(mtime, xfrom, mdel)"
    " VALUES(julianday('now'), %Q, %d);\n"
    "COMMIT;",
    mdel, g.zLogin, mdel
  );
}

/*
** WEBPAGE: chat-ping
**
** HTTP requests coming to this page from a loopback IP address cause
** a single \007 (bel) character to be written on the controlling TTY.
** This is used to implement an audiable alert by local web clients.
*/
void chat_ping_webpage(void){
  const char *zIpAddr = PD("REMOTE_ADDR","nil");
  if( cgi_is_loopback(zIpAddr) ){
    cgi_append_header("Access-Control-Allow-Origin: *\r\n");
    fputc(7, stderr);
  }
}

/*
** COMMAND: chat
**
** Usage: %fossil chat ?URL?
**
** Bring up a window to the chatroom feature of the Fossil repository
** at URL.  Or if URL is not specified, use the default remote repository.
** Event notifications on this session cause the U+0007 character to
** be sent to the TTY on which the "fossil chat" command is run, thus
** causing an auditory notification.
*/
void chat_command(void){
  const char *zUrl = 0;
  size_t i;
  char *azArgv[5];
  db_find_and_open_repository(0,0);
  if( g.argc==3 ){
    zUrl = g.argv[2];
  }else if( g.argc!=2 ){
    usage("?URL?");
  }else{
    zUrl = db_get("last-sync-url",0);
    if( zUrl==0 ){
      fossil_fatal("no \"remote\" repository defined.  Use a URL argument");
    }
    url_parse(zUrl, 0);
    if( g.url.port==g.url.dfltPort ){
      zUrl = mprintf(
        "%s://%T%T",
        g.url.protocol, g.url.name, g.url.path
      );
    }else{
      zUrl = mprintf(
        "%s://%T:%d%T",
        g.url.protocol, g.url.name, g.url.port, g.url.path
      );
    }
  }
  if( strncmp(zUrl,"http://",7)!=0 && strncmp("https://",zUrl,8)!=0 ){
    fossil_fatal("Not a valid URL: %s", zUrl);
  }
  azArgv[0] = g.argv[0];
  azArgv[1] = "ui";
  azArgv[2] = "--internal-chat-url";
  i = strlen(zUrl);
  if( i && zUrl[i-1]=='/' ) i--;
  azArgv[3] = mprintf("%.*s/chat?ping=%%d", i, zUrl);
  azArgv[4] = 0;
  g.argv = azArgv;
  g.argc = 4;
  cmd_webserver();
}
