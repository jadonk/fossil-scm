# Proxying Fossil via HTTPS with nginx

One of the [many ways](./ssl.wiki) to provide TLS-encrypted HTTP access
(a.k.a. HTTPS) to Fossil is to run it behind a web proxy that supports
TLS. This document explains how to use the powerful [nginx web
server](http://nginx.org/) to do that.


## Benefits

This scheme is complicated, even with the benefit of this guide and
pre-built binary packages. Why should you put up with this complexity?
Because it gives many benefits that are difficult or impossible to get
with the less complicated options:

*   **Power** — nginx is one of the most powerful web servers in the
    world. The chance that you will run into a web serving wall that you
    can’t scale with nginx is very low.

    To give you some idea of the sort of thing you can readily
    accomplish with nginx, your author runs a single public web server
    that provides transparent name-based virtual hosting for four
    separate domains:

    *   One is entirely static, not involving any dynamic content or
        Fossil integration at all.

    *   Another is served almost entirely by Fossil, with a few select
        static content exceptions punched past Fossil, which are handled
        entirely via nginx.

    *   The other two domains are aliases for one another — e.g.
        `example.com` and `example.net` — with most of the content being
        static.  This pair of domains has three different Fossil repo
        proxies attached to various sections of the URI hierarchy.

    All of this is done with minimal configuration repetition between
    the site configurations.

*   **Integration** — Because nginx is so popular, it integrates with
many different technologies, and many other systems integrate with it in
turn.  This makes it great middleware, sitting between the outer web
world and interior site services like Fossil. It allows Fossil to
participate seamlessly as part of a larger web stack.

*   **Availability** — nginx is already in most operating system binary
package repositories, so you don’t need to go out of your way to get it.


## Fossil Remote Access Methods

Fossil provides four major ways to access a repository it’s serving
remotely, three of which you can use with nginx:

*   **HTTP** — Fossil has a built-in HTTP server: `fossil server`.
    While this method is efficient and it’s possible to use nginx to
    proxy access to another HTTP server, this option is overkill for our
    purposes.  nginx is itself a fully featured HTTP server, so we will
    choose in this guide not to make nginx reinterpret Fossil’s
    implementation of HTTP.

*   **CGI** — This method is simple but inefficient, because it launches
    a separate Fossil instance on every HTTP hit.
    
    Since Fossil is a relatively small self-contained program, and it’s
    designed to start up quickly, this method can work well in a
    surprisingly large number of cases.

    Nevertheless, we will avoid this option in this document because
    we’re already buying into a certain amount of complexity here in
    order to gain power.  There’s no sense in throwing away any of that
    hard-won performance on CGI overhead.

*   **SCGI** — The [SCGI protocol][scgi] provides the simplicity of CGI
    without its performance problems.

*   **SSH** — This method exists primarily to avoid the need for HTTPS
    in the first place.  There is probably a way to get nginx to proxy
    Fossil to HTTPS via SSH, but it would be pointlessly complicated.

SCGI it is, then.


# Installing

The first step is to install the pieces we’ll be working with.  This
varies on different operating systems, so to avoid overcomplicating this
guide, we’re going to assume you’re using Ubuntu Server 18.04 LTS, a
common Tier 1 offering for [virtual private servers][vps].

SSH into your server, then say:

       $ sudo apt install certbot fossil nginx

For other operating systems, simply visit [the front Certbot web
page][cb] and tell it what OS and web stack you’re using. Chances are
good that they’ve got a good guide for you already.


# Running Fossil in SCGI Mode

You presumably already have a working Fossil configuration on the public
server you’re trying to set up and are just following this guide to
replace HTTP service with HTTPS.

(You can adjust the advice in this guide to get both HTTP *and* HTTPS
service on the same site, but I strongly recommend that you do not do
that: the good excuses remaining for continuing to allow HTTP on public
web servers are running thin these days.)

I run my Fossil SCGI server instances with a variant of [the `fslsrv`
shell script](/file/tools/fslsrv) currently hosted in the Fossil source
code repository. You’ll want to download that and make a copy of it, so
you can customize it to your particular needs.

This script allows running multiple Fossil SCGI servers, one per
repository, each bound to a different high-numbered `localhost` port, so
that only nginx can see and proxy them out to the public.  The
“`example`” repo is on TCP port localhost:12345, and the “`foo`” repo is
on localhost:12346.

As written, the `fslsrv` script expects repositories to be stored in the
calling user’s home directory under `~/museum`, because where else do
you keep Fossils?

That home directory also needs to have a directory to hold log files,
`~/log/fossil/*.log`. Fossil doesn’t put out much logging, but when it
does, it’s better to have it captured than to need to re-create the
problem after the fact.

The use of `--baseurl` in this script lets us have each Fossil
repository mounted in a different location in the URL scheme.  Here, for
example, we’re saying that the “`example`” repository is hosted under
the `/code` URI on its domains, but that the “`foo`” repo is hosted at
the top level of its domain.  You’ll want to do something like the
former for a Fossil repo that’s just one piece of a larger site, but the
latter for a repo that is basically the whole point of the site.

This script’s automatic restart feature makes Fossil upgrades easy:

       $ cd ~/src/fossil/trunk ; fossil up ; make ; killall fossil ;
         sudo make install ; fslsrv

I’ve written that as a single long command because I keep it in the
history for my Fossil servers, so I can just run it again from history.
You could put it in a shell script instead.

The `killall fossil` step is needed only on OSes that refuse to let you
replace a running binary on disk.

As written, the `fslsrv` script assumes a Linux environment.  It expects
`/bin/bash` to exist, and it depends on non-POSIX tools like `pgrep`.
It shouldn’t be difficult to port to very different systems, like macOS
or the BSDs.


# Configuring Let’s Encrypt, the Easy Way

If your web serving needs are simple, [Certbot][cb] can configure nginx
for you and keep its certificates up to date.  The details are pretty
much as in the Certbot documentation for [nginx on Ubuntu 18.04 LTS
guide][cbnu], except that where they recommend that you use the
first-party Certbot packages, we’ve found that the ones that come with
Ubuntu work just fine.

The primary local configuration you need is to tell nginx how to proxy
certain URLs down to the Fossil instance you started above with the
`fslsrv` script:

      location / {
           include scgi_params;
           scgi_pass 127.0.0.1:12345;
           scgi_param HTTPS "on";
           scgi_param SCRIPT_NAME "";
      }

The TCP port number in that snippet is the key: it has to match the port
number generated by `fslsrv` from the base port number passed to the
`start_one` function.


# Configuring Let’s Encrypt, the Hard Way

If you’re finding that you can’t get certificates to be issued or
renewed using the Easy Way instructions, the problem is usually that
your nginx configuration is too complicated for Certbot’s `--nginx`
plugin to understand. It attempts to rewrite your nginx configuration
files on the fly to achieve the renewal, and if it doesn’t put its
directives in the right locations, the ACME verification steps can fail.

Your author’s configuration, glossed above, is complicated enough that
the current version of Certbot (0.28 at the time of this writing) can’t
cope with it.  That’s the primary motivation for me to write this guide:
I’m addressing the “me” years hence who needs to upgrade to Ubuntu 20.04
or 22.04 LTS and has forgotten all of this stuff. 😉


## Step 1: Shifting into Manual

The first thing to do is to turn off all of the Certbot automation,
because it’ll only get in our way.  First, disable the Certbot package’s
automatic background updater:

      $ sudo systemctl disable certbot.timer

Next, edit `/etc/letsencrypt/renewal/example.com.conf` to disable the
nginx plugins. You’re looking for two lines setting the “install” and
“auth” plugins to “nginx”.  You can comment them out or remove them
entirely.


## Step 2: Configuring nginx

On Ubuntu systems, at least, the primary user-level configuration file
is `/etc/nginx/sites-enabled/default`. For a configuration like I
described at the top of this article, I recommend that this file contain
only a list of include statements, one for each site that server hosts:

      include local/example
      include local/foo

Those files then each define one domain’s configuration.  Here,
`/etc/nginx/local/example` contains the configuration for
`*.example.com` and `*.example.net`; and `local/foo` contains the
configuration for `*.foo.net`.

Here’s an example configuration:

      server {
          server_name .foo.net;
      
          include local/tls-common;
      
          charset utf-8;
      
          access_log /var/log/nginx/foo.net-https-access.log;
           error_log /var/log/nginx/foo.net-https-error.log;
      
          # Bypass Fossil for the static Doxygen docs
          location /doc/html {
              root /var/www/foo.net;
      
              location ~* \.(html|ico|css|js|gif|jpg|png)$ {
                  expires 7d;
                  add_header Vary Accept-Encoding;
                  access_log off;
              }
          }
      
          # Redirect everything else to the Fossil instance
          location / {
              include scgi_params;
              scgi_pass 127.0.0.1:12345;
              scgi_param HTTPS "on";
              scgi_param SCRIPT_NAME "";
          }
      }
      server {
          server_name .foo.net;
          root /var/www/foo.net;
          include local/http-certbot-only;
          access_log /var/log/nginx/foo.net-http-access.log;
           error_log /var/log/nginx/foo.net-http-error.log;
      }

Notice that we need two `server { }` blocks: one for HTTPS service, and
one for HTTP-only service:


### HTTP over TLS (HTTPS) Service

The first `server { }` block includes this file, `local/tls-common`:

      listen 443 ssl;

      ssl_certificate     /etc/letsencrypt/live/example.com/fullchain.pem;
      ssl_certificate_key /etc/letsencrypt/live/example.com/privkey.pem;

      ssl_dhparam /etc/letsencrypt/ssl-dhparams.pem;

      ssl_protocols TLSv1 TLSv1.1 TLSv1.2;
      ssl_ciphers "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES128-GCM-SH
      ssl_session_cache shared:le_nginx_SSL:1m;
      ssl_prefer_server_ciphers on;
      ssl_session_timeout 1440m;

These are the common TLS configuration parameters used by all domains
hosted by this server. Since all of those domains share a single TLS
certificate, we reference the same `example.com/*.pem` files written out
by Certbot here. We also reference the common server-specific
Diffie-Hellman parameter file written by the Let’s Encrypt package when
it’s initially installed.

The `ssl_protocols` and `ssl_ciphers` lines are prone to bit-rot: as new
attacks on TLS and its associated technologies are discovered, this
configuration is likely to need to change. Even if we fully succeed in
[keeping this document up-to-date](#evolution), the nature of this guide
is to recommend static configurations for your server. You will have to
keep an eye on this sort of thing and evolve your local configuration as
the world changes around it.

Running a TLS certificate checker against your site occasionally is a
good idea. The most thorough service I’m aware of is the [Qualys SSL
Labs Test][qslt], which gives the site I’m basing this guide on an “A”
rating at the time of this writing.

I assume you’re taking my advice to serve only the least amount of stuff
over HTTP that you can get away with. Certbot’s ACME HTTP-01
authentication scheme is one of those few things.


### HTTP-Only Service

While we’d prefer not to offer HTTP service at all, we need to do so for
two reasons, one temporary and the other going forward indefinitely.

First, until we get Let’s Encrypt certificates minted and configured
properly, we can’t use HTTPS yet at all.

Second, the Certbot ACME HTTP-01 challenge used by the Let’s Encrypt
service only runs over HTTP, because it has to work before HTTPS is
working, or after a certificate is accidentally allowed to lapse.  This
is the protocol Let’s Encrypt uses to determine whether we actually have
control over the domains we want our certificate to be minted for.
Let’s Encrypt will not just let you mint certificates for `google.com`
and `paypal.com`!

So, from the second `service { }` block, we include this file to set up
the minimal HTTP service we reqiure, `local/http-certbot-only`:

      listen 80;
      listen [::]:80;
  
      # This is expressed as a rewrite rule instead of an "if" because
      # http://wiki.nginx.org/IfIsEvil
      #rewrite ^(/.well-known/acme-challenge/.*) $1 break;
  
      # Force everything else to HTTPS with a permanent redirect.
      #return 301 https://$host$request_uri;

As written above, this configuration does nothing other than to tell
nginx that it’s allowed to serve content via HTTP on port 80 as well.

We’ll uncomment the `rewrite` and `return` directives below, when we’re
ready to begin testing.


#### Why the Repitition?

You need to do much the same sort of thing as above for each domain name
hosted by your nginx server.

You might being to wonder, then, why I haven’t factored some of those
directives into the included files `local/tls-common` and
`local/http-certbot-only`. For example, why can’t the second HTTP-only
`server { }` block above just be these two lines:

      server_name .foo.net;
      include local/http-certbot-only;

Then in `local/http-certbot-only`, we’d like to say:

      root /var/www/$host;
      access_log /var/log/nginx/$host-http-access.log;
       error_log /var/log/nginx/$host-http-error.log;

Sadly, nginx doesn’t allow variable subtitution into any of these
directives. As I understand it, allowing that would make nginx slower,
so we must largely repeat these directives in each HTTP `server { }`
block.

These configurations are, as shown, as small as I know how to get them.
If you know of a way to reduce some of this repitition, [I solicit your
advice][fd].


## Step 3: Dry Run

We want to first request a dry run, because Let’s Encrypt puts some
rather low limits on how often you’re allowed to request an actual
certificate.  You want to be sure everything’s working before you do
that.  You’ll run a command something like this:

      $ sudo certbot certonly --webroot --dry-run \
         --webroot-path /var/www/example.com \
             -d example.com -d www.example.com \
             -d example.net -d www.example.net \
         --webroot-path /var/www/foo.net \
             -d foo.net -d www.foo.net

There are two key options here.

First, we’re telling Certbot to use its `--webroot` plugin instead of
the automated `--nginx` plugin. With this plugin, Certbot writes the
ACME HTTP-01 challenge files to the static web document root directory
behind each domain.  For this example, we’ve got two web roots, one of
which holds documents for two different second-level domains
(`example.com` and `example.net`) with `www` at the third level being
optional.  This is a common sort of configuration these days, but you
needn’t feel that you must slavishly imitate it; the other web root is
for an entirely different domain, also with `www` being optional.  Since
all of these domains are served by a single nginx instance, we need to
give all of this in a single command, because we want to mint a single
certificate that authenticates all of these domains.

The second key option is `--dry-run`, which tells Certbot not to do
anything permanent.  We’re just seeing if everything works as expected,
at this point.


### Troubleshooting the Dry Run

If that didn’t work, try creating a manual test:

      $ mkdir -p /var/www/example.com/.well-known/acme-challenge
      $ echo hi > /var/www/example.com/.well-known/acme-challenge/test

Then try to pull that file over HTTP — not HTTPS! — as
`http://example.com/.well-known/acme-challenge/test`. I’ve found that
using Firefox and Safari is better for this sort of thing than Chrome,
because Chrome is more aggressive about automatically forwarding URLs to
HTTPS even if you requested “`http`”.

In extremis, you can do the test manually:

      $ telnet foo.net 80
      GET /.well-known/acme-challenge/test HTTP/1.1
      Host: example.com

      HTTP/1.1 200 OK
      Server: nginx/1.14.0 (Ubuntu)
      Date: Sat, 19 Jan 2019 19:43:58 GMT
      Content-Type: application/octet-stream
      Content-Length: 3
      Last-Modified: Sat, 19 Jan 2019 18:21:54 GMT
      Connection: keep-alive
      ETag: "5c436ac2-4"
      Accept-Ranges: bytes

      hi

You’re looking for that “hi” line at the end and the “200 OK” response
here. If you get a 404 or other error response, you need to look into
your web server logs to find out what’s going wrong.

Note that it’s important to do this test with HTTP/1.1 when debugging a
name-based virtual hosting configuration like this, if the test domain
is one of the secondary names, as in the example above, `foo.net`.

If you’re still running into trouble, the log file written by Certbot
can be helpful.  It tells you where it’s writing it early in each run.



## Step 4: Getting Your First Certificate

Once the dry run is working, you can drop the `--dry-run` option and
re-run the long command above.  (The one with all the `--webroot*`
flags.) This should now succeed, and it will save all of those flag
values to your Let’s Encrypt configuration file, so you don’t need to
keep giving them.



## Step 5: Test It

Edit the `local/http-certbot-only` file and uncomment the `redirect` and
`return` directives, then restart your nginx server and make sure it now
forces everything to HTTPS like it should:

      $ sudo systemctl restart nginx

Test ideas:

*   Visit both Fossil and non-Fossil URLs

*   Log into the repo, log out, and log back in

*   Clone via `http`: ensure that it redirects to `https`, and that
    subsequent `fossil sync` commands go directly to `https` due to the
    301 permanent redirect.

This forced redirect is why we don’t need the Fossil Admin &rarr; Access
"Redirect to HTTPS on the Login page" setting to be enabled.  Not only
is it unnecessary with this HTTPS redirect at the front-end proxy level,
it would actually [cause an infinite redirect loop if
enabled](./ssl.wiki#rloop).


## Step 6: Renewing Automatically

Now that the configuration is solid, you can renew the LE cert and
restart nginx with two short commands, which are easily automated:

      sudo certbot certonly --webroot
      sudo systemctl restart nginx

I put those in a script in the `PATH`, then arrange to call that
periodically.  Let’s Encrypt doesn’t let you renew the certificate very
often unless forced, and when forced there’s a maximum renewal counter.
Nevertheless, some people recommend running this daily and just letting
it fail until the server lets you renew.  Others arrange to run it no
more often than it’s known to work without complaint.  Suit yourself.


-----------

<a id=”evolution”></a>
**Document Evolution**

TLS and web proxying are a constantly evolving technology. This article
replaces my [earlier effort][2016], which had whole sections that were
basically obsolete within about a year of posting it. Two years on, and
I was encouraging readers to ignore about half of that HOWTO.  I am now
writing this document about 3 years later because Let’s Encrypt
deprecated key technology that HOWTO depended on, to the point that
following that old HOWTO is more likely to confuse than enlighten.

There is no particularly good reason to expect that this sort of thing
will not continue to happen, so this effort is expected to be a living
document.  If you do not have commit access on the `fossil-scm.org`
repository to update this document as the world changes around it, you
can discuss this document [on the forum][fd].  This document’s author
keeps an eye on the forum and expects to keep this document updated with
ideas that appear in that thread.

[2016]: https://www.mail-archive.com/fossil-users@lists.fossil-scm.org/msg22907.html
[cb]:   https://certbot.eff.org/
[cbnu]: https://certbot.eff.org/lets-encrypt/ubuntubionic-nginx
[fd]:   https://fossil-scm.org/forum/forumpost/XXXXXXXX
[qslt]: https://www.ssllabs.com/ssltest/
[scgi]: https://en.wikipedia.org/wiki/Simple_Common_Gateway_Interface
[vps]:  https://en.wikipedia.org/wiki/Virtual_private_server
