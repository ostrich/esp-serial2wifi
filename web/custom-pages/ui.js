// Helpers for custom device pages. The admin interface is /index.html.
//===== Collection of small utilities

/*
 * Bind/Unbind events
 *
 * Usage:
 *   var el = document.getElementyById('#container');
 *   bnd(el, 'click', function() {
 *     console.log('clicked');
 *   });
 */

var bnd = function (
  d, // a DOM element
  e, // an event name such as "click"
  f // a handler function
) {
  d.addEventListener(e, f, false);
};

/*
 * Create DOM element
 *
 * Usage:
 *   var el = m('<h1>Hello</h1>');
 *   document.body.appendChild(el);
 *
 * Copyright (C) 2011 Jed Schmidt <http://jed.is> - WTFPL
 * More: https://gist.github.com/966233
 */

var m = function (
  a, // an HTML string
  b, // placeholder
  c // placeholder
) {
  b = document; // get the document,
  c = b.createElement("p"); // create a container element,
  c.innerHTML = a; // write the HTML to it, and
  a = b.createDocumentFragment(); // create a fragment.

  while (
    // while
    (b = c.firstChild) // the container element has a first child
  )
    a.appendChild(b); // append the child to the fragment,

  return a; // and then return the fragment.
};

/*
 * DOM selector
 *
 * Usage:
 *   $('div');
 *   $('#name');
 *   $('.name');
 *
 * Copyright (C) 2011 Jed Schmidt <http://jed.is> - WTFPL
 * More: https://gist.github.com/991057
 */

var $ = function (
  a, // take a simple selector like "name", "#name", or ".name", and
  b // an optional context, and
) {
  a = a.match(/^(\W)?(.*)/); // split the selector into name and symbol.
  return (
    // return an element or list, from within the scope of
    (b || // the passed context
      document) // or document,
      [
        "getElement" + // obtained by the appropriate method calculated by
          (a[1]
            ? a[1] == "#"
              ? "ById" // the node by ID,
              : "sByClassName" // the nodes by class name, or
            : "sByTagName") // the nodes by tag name,
      ](
        a[2] // called with the name.
      )
  );
};

var j = function () {
  return new XMLHttpRequest();
};

// dom element iterator: domForEach($(".some-class"), function(el) { ... });
function domForEach(els, fun) {
  return Array.prototype.forEach.call(els, fun);
}

// createElement short-hand

e = function (a) {
  return document.createElement(a);
};

// chain onload handlers

function onLoad(f) {
  var old = window.onload;
  if (typeof old != "function") {
    window.onload = f;
  } else {
    window.onload = function () {
      old();
      f();
    };
  }
}

//===== helpers to add/remove/toggle HTML element classes

function addClass(el, cl) {
  el.className += " " + cl;
}
function removeClass(el, cl) {
  var cls = el.className.split(/\s+/),
    l = cls.length;
  for (var i = 0; i < l; i++) {
    if (cls[i] === cl) cls.splice(i, 1);
  }
  el.className = cls.join(" ");
  return cls.length != l;
}
function toggleClass(el, cl) {
  if (!removeClass(el, cl)) addClass(el, cl);
}

//===== AJAX

function ajaxReq(method, url, ok_cb, err_cb, data) {
  var xhr = j(),
    settled = false,
    timer;
  function finish(ok, status, text) {
    if (settled) return;
    settled = true;
    clearTimeout(timer);
    (ok ? ok_cb || function () {} : err_cb || function () {})(ok ? text : status, text);
  }
  xhr.open(method, url, true);
  xhr.onreadystatechange = function () {
    if (xhr.readyState === 4)
      finish(xhr.status >= 200 && xhr.status < 300, xhr.status, xhr.responseText);
  };
  xhr.onerror = function () {
    finish(false, 599, "connection failed");
  };
  xhr.onabort = function () {
    finish(false, 599, "request cancelled");
  };
  timer = setTimeout(function () {
    finish(false, 599, "request time-out");
    xhr.abort();
  }, 9000);
  try {
    xhr.send(data);
  } catch (err) {
    finish(false, 599, "request failed");
  }
  return xhr;
}

function dispatchJson(resp, ok_cb, err_cb) {
  err_cb = err_cb || function () {};
  var j;
  try {
    j = JSON.parse(resp);
  } catch (err) {
    console.log("JSON parse error: " + err + ". In: " + resp);
    err_cb(500, "JSON parse error: " + err);
    return;
  }
  ok_cb(j);
}

function ajaxJson(method, url, ok_cb, err_cb) {
  return ajaxReq(
    method,
    url,
    function (resp) {
      dispatchJson(resp, ok_cb, err_cb);
    },
    err_cb
  );
}

var spinnerRequests = 0;
function ajaxSpin(method, url, ok_cb, err_cb) {
  spinnerRequests++;
  $("#spinner").removeAttribute("hidden");
  function done() {
    if (--spinnerRequests === 0) $("#spinner").setAttribute("hidden", "");
  }
  return ajaxReq(
    method,
    url,
    function (resp) {
      done();
      if (ok_cb) ok_cb(resp);
    },
    function (status, text) {
      done();
      if (err_cb) err_cb(status, text);
    }
  );
}

function ajaxJsonSpin(method, url, ok_cb, err_cb) {
  ajaxSpin(
    method,
    url,
    function (resp) {
      dispatchJson(resp, ok_cb, err_cb);
    },
    err_cb
  );
}

// Custom-page shell, notifications and navigation. No admin configuration forms.
var notifTimeout = null;
function showWarning(text) {
  var el = $("#warning");
  if (el) {
    el.textContent = text;
    el.hidden = false;
  }
}
function hideWarning() {
  var el = $("#warning");
  if (el) el.hidden = true;
}
function showNotification(text) {
  var el = $("#notification");
  if (!el) return;
  clearTimeout(notifTimeout);
  el.textContent = text;
  el.hidden = false;
  notifTimeout = setTimeout(function () {
    el.hidden = true;
    notifTimeout = null;
  }, 4000);
}
onLoad(function () {
  var icon = e("link");
  icon.rel = "icon";
  icon.type = "image/x-icon";
  icon.href = "/favicon.ico?v=serial2wifi";
  document.head.appendChild(icon);
  var layout = $("#layout");
  if (!layout) return;
  var shell = m(
    '<div id="spinner" class="spinner" hidden></div><div id="messages"><div id="warning" hidden></div><div id="notification" hidden></div></div><a href="#menu" id="menuLink" class="menu-link"><span></span></a><div id="menu"><div class="pure-menu"><a class="pure-menu-heading" href="/">ESP Serial2WiFi</a><div class="pure-menu-heading system-name"></div><ul id="menu-list" class="pure-menu-list"></ul></div></div>'
  );
  layout.insertBefore(shell, layout.firstChild);
  bnd($("#menuLink"), "click", function (event) {
    event.preventDefault();
    toggleClass(layout, "active");
    toggleClass($("#menu"), "active");
    toggleClass($("#menuLink"), "active");
  });
  var failures = 0;
  function loadMenu() {
    ajaxJson(
      "GET",
      "/menu",
      function (data) {
        var list = $("#menu-list");
        list.textContent = "";
        for (var i = 0; i < data.menu.length; i += 2) {
          var li = e("li"),
            link = e("a"),
            path = data.menu[i + 1];
          if (typeof path !== "string" || path.charAt(0) !== "/" || path.slice(0, 2) === "//")
            continue;
          li.className =
            "pure-menu-item" + (location.pathname === path ? " pure-menu-selected" : "");
          link.className = "pure-menu-link";
          link.href =
            path.indexOf("/index.html#/") === 0
              ? path
              : "/" + path.slice(1).split("/").map(encodeURIComponent).join("/");
          link.textContent = data.menu[i];
          li.appendChild(link);
          list.appendChild(li);
        }
        document.title = data.name || "ESP Serial2WiFi";
        domForEach($(".system-name"), function (el) {
          el.textContent = data.name || "ESP Serial2WiFi";
        });
        var version = $("#version");
        if (version) version.textContent = data.version;
      },
      function () {
        if (++failures < 3) setTimeout(loadMenu, 1000);
        else showWarning("Navigation unavailable. Open / for device management.");
      }
    );
  }
  loadMenu();
});
