//===== Java script for user pages

var loadCounter = 0;
var refreshRate = 0;
var refreshTimer;
var hiddenInputs = [];
var pageActive=true,refreshFlight=null,refreshPending=false,refreshFailures=0,submitTimer;
function fetchFormData(reason){
  if(!pageActive)return;
  if(refreshFlight){refreshPending=true;return;}
  clearTimeout(refreshTimer);
  var own={xhr:null};refreshFlight=own;
  own.xhr=ajaxJson('GET',window.location.pathname+'.json?reason='+reason,function(resp){
    if(refreshFlight!==own)return;refreshFlight=null;if(!pageActive)return;refreshFailures=0;notifyResponse(resp);
    if(loadCounter>0&&reason==='refresh'){loadCounter--;refreshFormData();}
    if(refreshPending){refreshPending=false;refreshFormData();}
  },function(){if(refreshFlight!==own)return;refreshFlight=null;if(!pageActive)return;
    if(reason==='load'||++refreshFailures<3)refreshTimer=setTimeout(function(){fetchFormData(reason);},1000);
    else showWarning('Page data is unavailable. Reload this page to try again.');
  });
}
window.addEventListener('pagehide',function(){pageActive=false;clearTimeout(refreshTimer);clearTimeout(submitTimer);refreshFlight&&refreshFlight.xhr&&refreshFlight.xhr.abort();refreshFlight=null;refreshPending=false;});
window.addEventListener('pageshow',function(event){if(event.persisted){pageActive=true;refreshFailures=0;fetchFormData('refresh');}});

function notifyResponse( data )
{
  if(!pageActive)return;
  Object.keys(data).forEach(function(v) {
    var elems = document.getElementsByName(v);
    var ndx;
    for(ndx = 0; ndx < elems.length; ndx++ )
    {
      var el = elems[ndx];
      if(el.tagName == "INPUT")
      {
        if( el.type == "radio" )
        {
          el.checked = data[v] == el.value;
        }
        else if( el.type == "checkbox" )
        {
          if( data[v] == "on" )
            el.checked = true;
          else if( data[v] == "off" )
            el.checked = false;
          else if( data[v] == true )
            el.checked = true;
          else
            el.checked = false;
        }
        else
        {
          el.value = data[v];
        }
      }
      if(el.tagName == "SELECT")
      {
        el.value = data[v];
      }
    }
    var elem = document.getElementById(v);
    if( elem != null )
    {
      if(elem.tagName == "P" || elem.tagName == "DIV" || elem.tagName == "SPAN" || elem.tagName == "TR" || elem.tagName == "TH" || elem.tagName == "TD" ||
         elem.tagName == "TEXTAREA" )
      {
        elem.innerHTML = data[v];
      }
      if(elem.tagName == "UL" || elem.tagName == "OL")
      {
        var list = data[v];
        var html = "";

        for (var i=0; i<list.length; i++) {
          html = html.concat("<li>" + list[i] + "</li>");
        }

        elem.innerHTML = html;
      }
      if(elem.tagName == "TABLE")
      {
        var list = data[v];
        var html = "";

        if( list.length > 0 )
        {
          var ths = list[0];
          html = html.concat("<tr>");

          for (var i=0; i<ths.length; i++) {
            html = html.concat("<th>" + ths[i] + "</th>");
          }

          html = html.concat("</tr>");
        }

        for (var i=1; i<list.length; i++) {
          var tds = list[i];
          html = html.concat("<tr>");

          for (var j=0; j<tds.length; j++) {
            html = html.concat("<td>" + tds[j] + "</td>");
          }

          html = html.concat("</tr>");
        }

        elem.innerHTML = html;
      }
    }
  });

  if( refreshRate != 0 )
  {
    clearTimeout(refreshTimer);
    refreshTimer = setTimeout( function () {
      fetchFormData('refresh');
    }, refreshRate );
  }
}

function notifyButtonPressed( btnId )
{
  ajaxJson("POST", window.location.pathname + ".json?reason=button\&id=" + encodeURIComponent(btnId), notifyResponse);
}

function refreshFormData()
{
  clearTimeout(submitTimer);
  submitTimer=setTimeout(function(){fetchFormData('refresh');},250);
}

function recalculateHiddenInputs()
{
  for(var i=0; i < hiddenInputs.length; i++)
  {
    var hinput = hiddenInputs[i];
    var name = hinput.name;

    var elems = document.getElementsByName(name);
    for(var j=0; j < elems.length; j++ )
    {
      var chk = elems[j];
      var inptp = chk.type;
      if( inptp == "checkbox" ) {
        if( chk.checked )
        {
          hinput.disabled = true;
          hinput.value = "on";
        }
        else
        {
          hinput.disabled = false;
          hinput.value = "off";
        }
      }
    }
  }
}

document.addEventListener("DOMContentLoaded", function(){
  // collect buttons
  var btns = document.getElementsByTagName("button");
  var ndx;

  for (ndx = 0; ndx < btns.length; ndx++) {
    var btn = btns[ndx];
    var id = btn.getAttribute("id");
    var onclk = btn.getAttribute("onclick");
    var type = btn.getAttribute("type");

    if( id != null && onclk == null && type == "button" )
    {
      btn.onclick = notifyButtonPressed.bind(null, id);
    }
  }

  // collect forms
  var frms = document.getElementsByTagName("form");

  for (ndx = 0; ndx < frms.length; ndx++) {
    var frm = frms[ndx];


    frm.method = "POST";
    frm.action = window.location.pathname + ".json?reason=submit";

    frm.onsubmit = function () {
      loadCounter=4;
      recalculateHiddenInputs();
      refreshFormData();
      return true;
    };
  }

  // collect metas
  var metas = document.getElementsByTagName("meta");

  for (ndx = 0; ndx < metas.length; ndx++) {
    var meta = metas[ndx];
    if( meta.getAttribute("name") == "refresh-rate" )
    {
      refreshRate = meta.getAttribute("content");
    }
  }

  // collect checkboxes
  var inputs = document.getElementsByTagName("input");

  for (ndx = 0; ndx < inputs.length; ndx++) {
    var inp = inputs[ndx];
    if( inp.getAttribute("type") == "checkbox" )
    {
      var name = inp.getAttribute("name");
      var hasHidden = false;
      if( name != null )
      {
        var inpelems = document.getElementsByName(name);
        for(var i=0; i < inpelems.length; i++ )
        {
           var inptp = inpelems[i].type;
           if( inptp == "hidden" )
             hasHidden = true;
        }
      }

      if( !hasHidden )
      {
        var parent = inp.parentElement;

        var input = document.createElement("input");
        input.type = "hidden";
        input.name = inp.name;

	parent.appendChild(input);
        hiddenInputs.push(input);
      }
    }
  }

  // load variables at first time
  fetchFormData('load');
});
