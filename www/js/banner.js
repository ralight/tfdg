const xCoords = [];
const tPos = [];
let time = 0;


for (let i = 0; i <= 218; i++) {
  xCoords.push(i);
} 

for (let i = 0; i <= screen.width+365; i++) {
  tPos.push(i);
} 

var animate = function() {
  let sideTopR, sideBottomR;
  let sideTopL, sideBottomL;
  const frequency = 20;
  const height = 8;
  const topOffset = 20;
  const points = xCoords.map(x => {
    
    let y = Math.cos((x + time) / frequency) * height + topOffset;
    
    return [x, y];
  });
  const ppoints = tPos.map(x => {
    let y = Math.sin((x + time) / frequency) * 4 + 10;
    return [window.innerWidth-x, y];
  });
  
  const topPath = "M" + points.map(p => {
    if (p[0] === 218) {
      sideTopR = p[1];
      sideBottomR = p[1] + 25;
    }
    if (p[0] === 1) {
      sideTopL = p[1];
      sideBottomL = p[1] + 25;
    }
    return `${p[0]+140},${p[1]}`;
  }).join(" L");
  
  const bottomPath = "M" + points.map(p => {
    return `${p[0]+140},${p[1] + 25}`;
  }).join(" L");
  
  const midPath = "M" + points.map(p => {
    return `${p[0]+140},${p[1] + 20}`;
  }).join(" L");
  
  const sidePathR = `M358,${sideTopR} L358,${sideBottomR}`;
  const sidePathL = `M141,${sideTopL} L141,${sideBottomL}`;
  
  const towPathTop = `M98,${25+ppoints[0][1]} L140,${sideTopL}`;
  const towPathBottom = `M98,${25+ppoints[0][1]} L140,${sideBottomL}`;
  
  document.querySelector("#bannerimg").setAttribute("viewBox", "0 0 " + window.innerWidth + " 65");
  document.querySelector(".flag-top").setAttribute("d", topPath);
  document.querySelector(".flag-middle").setAttribute("d", midPath);
  document.querySelector(".flag-bottom").setAttribute("d", bottomPath);
  document.querySelector(".flag-side-r").setAttribute("d", sidePathR);
  document.querySelector(".flag-side-l").setAttribute("d", sidePathL);
  document.querySelector(".tow-top").setAttribute("d", towPathTop);
  document.querySelector(".tow-bottom").setAttribute("d", towPathBottom);
  document.querySelector("#g833").setAttribute("transform", "translate(0 "+ppoints[0][1]+")");
  document.querySelector("#all").setAttribute("transform", "translate("+ppoints[-2*time][0] + " 0)");
  
  time -= 1;
  
  if(-2*time < window.innerWidth+365){
    requestAnimationFrame(animate);
  }
};
