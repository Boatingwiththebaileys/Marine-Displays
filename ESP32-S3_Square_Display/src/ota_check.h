#pragma once
// ota_check.h — "Check for updates" box for the Firmware Update page (/update).
//
// The display itself never goes on the internet (HTTPS would need ~40 KB of
// internal RAM it doesn't have). Instead the browser viewing the page:
//   1. reads the installer's manifest-<board>.json (stable, and beta/) from
//      GitHub Pages, which allows cross-site reads, to get the latest versions
//   2. on "Install", downloads that version's app.bin (the app-only image the
//      installer flashes at 0x10000, i.e. exactly an OTA image)
//   3. posts it to this display's /update, so the usual OtaGuard checks apply.
// Without internet on the phone/computer it just says so; manual upload still works.
//
// The same file is used by every display project; only the board key differs.

#include <Arduino.h>

static String ota_check_html(const char* board_key, const char* current_version) {
    String h;
    h.reserve(4200);
    h += F("<div id='updbox' style='border:1px solid #ccc;border-radius:6px;padding:10px 12px;margin:12px 0'>"
           "<h3 style='margin:0 0 6px'>Check for updates</h3>"
           "<p id='updst' style='margin:4px 0'>Checking the installer site&hellip;</p>"
           "<div id='updrows'></div>"
           "<progress id='updprog' max='100' value='0' style='width:100%;display:none'></progress>"
           "<p style='font-size:0.85em;color:#888;margin:6px 0 0'>Uses this phone or computer's internet "
           "connection. The display downloads nothing itself.</p></div>"
           "<script>(function(){");
    h += "var BOARD='"; h += board_key; h += "',CUR='"; h += current_version; h += "';";
    h += F(
        "var BASE='https://boatingwiththebaileys.github.io/Marine-Displays/';"
        "var st=document.getElementById('updst'),rows=document.getElementById('updrows'),"
            "prog=document.getElementById('updprog'),busy=false;"
        // v1.2.3 / v1.2.3-beta.4 -> [1,2,3,4] (a release sorts after its betas); else null
        "function pv(v){var m=/^v?(\\d+)\\.(\\d+)\\.(\\d+)(?:-beta\\.(\\d+))?$/.exec(v||'');"
            "return m?[+m[1],+m[2],+m[3],m[4]===undefined?1e9:+m[4]]:null;}"
        "function cmp(a,b){a=pv(a);b=pv(b);if(!a||!b)return null;"
            "for(var i=0;i<4;i++){if(a[i]!=b[i])return a[i]<b[i]?-1:1;}return 0;}"
        "function man(ch){var dir=ch=='beta'?'beta/':'';"
            "return fetch(BASE+dir+'manifest-'+BOARD+'.json',{cache:'no-store'})"
            ".then(function(r){return r.ok?r.json():null;})"
            ".then(function(m){if(!m||!m.version||m.version=='dev')return null;"
                "var ps=(m.builds&&m.builds[0]&&m.builds[0].parts)||[],app=null;"
                "for(var i=0;i<ps.length;i++)if(/app\\.bin$/.test(ps[i].path))app=ps[i].path;"
                "return app?{ch:ch,ver:m.version,url:BASE+dir+app}:null;})"
            ".catch(function(){return null;});}"
        "function row(r){var c=cmp(r.ver,CUR),note=c===0?' (installed)':c>0?' &mdash; newer':'';"
            "var d=document.createElement('div');d.style.margin='6px 0';"
            "d.innerHTML='<b>'+(r.ch=='beta'?'Beta':'Stable')+':</b> '+r.ver+note+' ';"
            "if(c!==0){var b=document.createElement('button');b.type='button';"
                "b.textContent=c<0?'Install older version':'Install';b.style.marginLeft='8px';"
                "b.onclick=function(){install(r);};d.appendChild(b);}"
            "rows.appendChild(d);}"
        "function install(r){if(busy)return;"
            "var older=cmp(r.ver,CUR)<0;"
            "if(!confirm('Install '+r.ver+' on this display? It will restart when done. Settings are kept.'+"
                "(older?'\\n\\nWarning: this is OLDER than the version installed ('+CUR+'). Older firmware may not "
                "understand settings saved by newer versions, so check your screens and settings afterwards.':'')))return;"
            "busy=true;st.textContent='Downloading '+r.ver+'\\u2026';"
            "fetch(r.url,{cache:'no-store'}).then(function(x){if(!x.ok)throw 0;return x.blob();})"
            ".then(function(blob){st.textContent='Sending to the display\\u2026 keep this page open.';"
                "prog.style.display='block';var fd=new FormData();fd.append('firmware',blob,'app.bin');"
                "var q=new XMLHttpRequest();q.open('POST','/update');"
                "q.upload.onprogress=function(e){if(e.lengthComputable)prog.value=e.loaded*100/e.total;};"
                "q.onload=function(){if(q.status==200){prog.value=100;"
                    "st.textContent='Installed '+r.ver+'. The display is restarting; this page reloads in 25 seconds.';"
                    "setTimeout(function(){location.href='/';},25000);}"
                    "else{document.open();document.write(q.responseText);document.close();}};"
                "q.onerror=function(){busy=false;st.textContent='Lost contact with the display while sending. "
                    "Check it is still on, then try again.';};"
                "q.send(fd);})"
            ".catch(function(){busy=false;st.textContent='Could not download '+r.ver+'. "
                "Check this phone or computer has internet, then try again.';});}"
        "Promise.all([man('stable'),man('beta')]).then(function(rs){"
            "rs=rs.filter(function(r){return r;});"
            "if(!rs.length){st.textContent='Can\\u2019t reach the installer site. This phone or computer "
                "needs internet access to check. You can still upload a file below.';return;}"
            "var newer=rs.filter(function(r){return cmp(r.ver,CUR)>0;});"
            "st.textContent=pv(CUR)?('This display has '+CUR+'. '+(newer.length?'An update is available.':'It is up to date.'))"
                ":('This display has a development build ('+CUR+').');"
            "rs.forEach(row);});"
        "})();</script>");
    return h;
}
