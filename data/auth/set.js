var websock;

function listCONF(obj) {
    document.getElementById("inputtohide").value = obj.ssid;
    document.getElementById("wifipass").value = obj.pswd;
    document.getElementById("gpioss").value = obj.sspin;
    document.getElementById("gain").value = obj.rfidgain;
    document.getElementById("gpiorly").value = obj.rpin;
    document.getElementById("delay").value = obj.rtime;
    if (obj.wmode === "1") {
      document.getElementById("wmodeap").checked = true;
    }
}

function listSSID(obj) {
    var select = document.getElementById("ssid");
    for (var i = 0; i < obj.ssid.length; i++) {
        var opt = document.createElement("option");
        opt.value = obj.ssid[i];
        opt.innerHTML = obj.ssid[i];
        select.appendChild(opt);
    }
    document.getElementById("scanb").innerHTML = "Re-Scan";
}

function scanWifi() {
    websock.send("{\"command\":\"scan\"}");
    document.getElementById("scanb").innerHTML = "...";
    document.getElementById("inputtohide").style.display = "none";
    var node = document.getElementById("ssid");
    node.style.display = "inline";
    while (node.hasChildNodes()) {
        node.removeChild(node.lastChild);
    }
}

function saveConf() {
    var ssid;
    if (document.getElementById("inputtohide").style.display === "none") {
        var b = document.getElementById("ssid");
        ssid = b.options[b.selectedIndex].value;
    } else {
        ssid = document.getElementById("inputtohide").value;
    }
    var wmode;
    if (document.getElementById("wmodeap").checked) {
      wmode = "1";
    }
    else {
      wmode = "0";
    }
    var datatosend = {};
    datatosend.command = "configfile";
    datatosend.ssid = ssid;
    datatosend.wmode = wmode;
    datatosend.pswd = document.getElementById("wifipass").value;
    datatosend.sspin = document.getElementById("gpioss").value;
    datatosend.rfidgain = document.getElementById("gain").value;
    datatosend.rpin = document.getElementById("gpiorly").value;
    datatosend.rtime = document.getElementById("delay").value;
    websock.send(JSON.stringify(datatosend));
    location.reload();
}

function testRelay() {
    websock.send("{\"command\":\"testrelay\"}");
}

function start() {
    websock = new WebSocket("ws://" + window.location.hostname + "/ws");
    websock.onopen = function(evt) {
        websock.send("{\"command\":\"getconf\"}");
    };
    websock.onclose = function(evt) {
    };
    websock.onerror = function(evt) {
        console.log(evt);
    };
    websock.onmessage = function(evt) {
        var obj = JSON.parse(evt.data);
        if (obj.command === "ssidlist") {
            listSSID(obj);
        } else if (obj.command === "configfile") {
            listCONF(obj);
            document.getElementById("loading-img").style.display = "none";
        }
    };
}
