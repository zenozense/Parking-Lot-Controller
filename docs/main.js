(function () {
  // Demo values for the simulator only. Change TOTAL to resize the lot.
  var TOTAL = 12;
  var parked = 5;

  var baysEl = document.getElementById('bays');
  var lcdEl = document.getElementById('lcd');
  var ledEl = document.getElementById('led');
  var gateEl = document.getElementById('gate');
  var gateText = document.getElementById('gateText');
  var btnIn = document.getElementById('btnIn');
  var btnOut = document.getElementById('btnOut');
  var statusText = document.getElementById('statusText');
  var fT = document.getElementById('fT');
  var fP = document.getElementById('fP');
  var fF = document.getElementById('fF');
  document.getElementById('totalLabel').textContent = TOTAL;

  // columns for the bay grid
  baysEl.style.gridTemplateColumns = 'repeat(' + Math.min(6, TOTAL) + ', minmax(0, 1fr))';

  var bays = [];
  for (var i = 0; i < TOTAL; i++) {
    var b = document.createElement('div');
    b.className = 'bay';
    var n = document.createElement('span');
    n.className = 'no';
    n.textContent = i + 1;
    b.appendChild(n);
    baysEl.appendChild(b);
    bays.push(b);
  }

  var cells = [];
  for (var c = 0; c < 32; c++) {
    var cell = document.createElement('span');
    cell.className = 'cell';
    lcdEl.appendChild(cell);
    cells.push(cell);
  }

  function pad(text) {
    // 16 characters per line like a 1602 LCD, centered
    var t = String(text).slice(0, 16);
    var left = Math.floor((16 - t.length) / 2);
    var s = '';
    for (var k = 0; k < left; k++) s += ' ';
    s += t;
    while (s.length < 16) s += ' ';
    return s;
  }

  function drawLcd(l1, l2) {
    var s = pad(l1) + pad(l2);
    for (var k = 0; k < 32; k++) cells[k].textContent = s.charAt(k) === ' ' ? '' : s.charAt(k);
  }

  function render() {
    var free = TOTAL - parked;
    for (var i = 0; i < TOTAL; i++) {
      var has = bays[i].querySelector('.car');
      if (i < parked && !has) {
        var car = document.createElement('div');
        car.className = 'car';
        bays[i].appendChild(car);
      } else if (i >= parked && has) {
        bays[i].removeChild(has);
      }
    }
    fT.textContent = TOTAL;
    fP.textContent = parked;
    fF.textContent = free;
    if (free === 0) {
      drawLcd('PARKING FULL', 'NO SLOT LEFT');
      ledEl.classList.add('full');
      statusText.textContent = 'เต็มแล้ว';
    } else {
      drawLcd('AVAILABLE SLOTS', free + ' / ' + TOTAL);
      ledEl.classList.remove('full');
      statusText.textContent = 'ว่าง ' + free + ' ช่อง';
    }
    btnIn.disabled = free === 0;
    btnOut.disabled = parked === 0;
  }

  var gateTimer = null;
  function openGate() {
    gateEl.classList.add('open');
    gateText.textContent = 'เปิด';
    clearTimeout(gateTimer);
    gateTimer = setTimeout(function () {
      gateEl.classList.remove('open');
      gateText.textContent = 'ปิด';
    }, 1600);
  }

  btnIn.addEventListener('click', function () {
    if (parked < TOTAL) { parked++; openGate(); render(); }
  });
  btnOut.addEventListener('click', function () {
    if (parked > 0) { parked--; render(); }
  });

  render();
})();