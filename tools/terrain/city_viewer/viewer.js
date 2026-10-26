import {createInspector} from "./inspector.js";
// Native generated triangles only. This client handles GPU upload and cameras.
const $=id=>document.getElementById(id), canvas=$('view');
const gl=canvas.getContext('webgl2',{antialias:true,alpha:false,preserveDrawingBuffer:true});
const state={position:[0,100,100],yaw:0,pitch:-.5,speed:12,keys:new Set(),drag:false,room:0,ready:false,frames:0,fps:0};
let inspector=null;
const QUAD_CORNERS=[0,1,2,0,2,3];
let instanceUniform;
let manifest, chunks=[], textures, program, projectionView=new Float32Array(16), activeBuilding=0;
const add=(a,b)=>a.map((v,i)=>v+b[i]), sub=(a,b)=>a.map((v,i)=>v-b[i]), mul=(a,s)=>a.map(v=>v*s);
const dot=(a,b)=>a.reduce((s,v,i)=>s+v*b[i],0), length=a=>Math.hypot(...a), normalize=a=>mul(a,1/Math.max(length(a),1e-8));
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
function multiply(a,b){const out=new Float32Array(16);for(let c=0;c<4;c++)for(let r=0;r<4;r++)for(let k=0;k<4;k++)out[c*4+r]+=a[k*4+r]*b[c*4+k];return out;}
function forward(){return [Math.sin(state.yaw)*Math.cos(state.pitch),Math.sin(state.pitch),-Math.cos(state.yaw)*Math.cos(state.pitch)];}
function lookAt(eye,target){state.position=[...eye];const d=normalize(sub(target,eye));state.pitch=Math.asin(d[1]);state.yaw=Math.atan2(d[0],-d[2]);state.keys.clear();}
function cameraMatrix(){const f=forward(),r=normalize(cross(f,[0,1,0])),u=cross(r,f),e=state.position;
 const view=new Float32Array([r[0],u[0],-f[0],0,r[1],u[1],-f[1],0,r[2],u[2],-f[2],0,-dot(r,e),-dot(u,e),dot(f,e),1]);
 const aspect=canvas.width/canvas.height,zoom=1/Math.tan(Math.PI*68/360),near=.06,far=manifest?.generation?.mode==='world-overview'?40000:4000;
 const projection=new Float32Array([zoom/aspect,0,0,0,0,zoom,0,0,0,0,(far+near)/(near-far),-1,0,0,2*far*near/(near-far),0]);
 return multiply(projection,view);
}
function shader(type,source){const s=gl.createShader(type);gl.shaderSource(s,source);gl.compileShader(s);if(!gl.getShaderParameter(s,gl.COMPILE_STATUS))throw Error(gl.getShaderInfoLog(s));return s;}
function makeProgram(){const p=gl.createProgram();gl.attachShader(p,shader(gl.VERTEX_SHADER,`#version 300 es
precision highp float;
layout(location=0) in vec3 position;layout(location=1) in vec2 uv;layout(location=2) in vec3 normal;layout(location=3) in float material;layout(location=4) in vec3 weights;
layout(location=5) in vec3 corner0;layout(location=6) in vec3 corner1;layout(location=7) in vec3 corner2;layout(location=8) in vec3 corner3;
layout(location=9) in vec2 uv0;layout(location=10) in vec2 uv1;layout(location=11) in vec2 uv2;layout(location=12) in vec2 uv3;layout(location=13) in float instanceMaterial;
uniform bool instanced;uniform mat4 matrix;out float vDepth;out vec3 vPosition;out vec2 vUV;out vec3 vNormal;out vec3 vWeights;flat out int vMaterial;
void main(){vPosition=position;vUV=uv;vNormal=normal;vWeights=weights;vMaterial=int(material+.5);
if(instanced){int ids[6]=int[6](0,1,2,0,2,3);vec3 corners[4]=vec3[4](corner0,corner1,corner2,corner3);vec2 uvs[4]=vec2[4](uv0,uv1,uv2,uv3);int k=ids[gl_VertexID];vPosition=corners[k];vUV=uvs[k];vNormal=normalize(cross(corner1-corner0,corner2-corner0));vWeights=vec3(0.);vMaterial=int(instanceMaterial+.5);}
gl_Position=matrix*vec4(vPosition,1.);vDepth=1.+gl_Position.w;}`));
 gl.attachShader(p,shader(gl.FRAGMENT_SHADER,`#version 300 es
precision highp float;precision highp sampler2DArray;
in float vDepth;in vec3 vPosition;in vec2 vUV;in vec3 vNormal;in vec3 vWeights;flat in int vMaterial;
uniform sampler2D groundMask;uniform bool hasGroundMask;uniform vec4 groundMaskBounds;uniform sampler2DArray atlas;uniform vec2 repeatSize[34];uniform vec3 materialTint[34];uniform vec3 eye;uniform bool textured;uniform float cutHeight;uniform float farDistance;uniform float fogDistance;
out vec4 color;
// Derivatives must precede fract: tile seams otherwise select pale coarse mip levels.
vec3 sampleMaterial(int id,vec2 uv){vec2 q=uv/repeatSize[id];vec2 dx=dFdx(q)*vec2(1.,-1.),dy=dFdy(q)*vec2(1.,-1.);q=id==8?clamp(q,vec2(.001),vec2(.999)):fract(q);q.y=1.-q.y;return textureGrad(atlas,vec3(q,float(id)),dx,dy).rgb*materialTint[id];}
// Logarithmic depth preserves millimetre-scale surface separation at city distances.
void main(){if(vPosition.y>cutHeight)discard;gl_FragDepth=log2(max(1e-6,vDepth))/log2(farDistance+1.);vec3 base;
 if(dot(vWeights,vec3(1.))>.5){vec3 w=vWeights/max(dot(vWeights,vec3(1.)),.001);if(hasGroundMask)w=texture(groundMask,(vPosition.xz-groundMaskBounds.xy)/groundMaskBounds.zw).rgb;base=sampleMaterial(11,vUV)*w.x+sampleMaterial(12,vUV)*w.y+sampleMaterial(13,vUV)*w.z;}

 // Masonry variation is authored per stone in the texture; never blend plaster across joints.
 else base=sampleMaterial(vMaterial,vUV);
 if(!textured)base=vec3(.64,.62,.56);
 vec3 n=normalize(vNormal);float diffuse=abs(dot(n,normalize(vec3(-.45,.8,.3))));float light=.52+.48*diffuse;
 float fog=1.-exp(-pow(length(vPosition-eye)/fogDistance,2.));color=vec4(mix(base*light,vec3(.64,.72,.73),fog),1.);}`));
 gl.linkProgram(p);if(!gl.getProgramParameter(p,gl.LINK_STATUS))throw Error(gl.getProgramInfoLog(p));return p;
}
async function response(url){const r=await fetch(url);if(!r.ok)throw Error(`${url}: HTTP ${r.status}`);return r;}
async function loadTextures(){const texture=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D_ARRAY,texture);gl.texStorage3D(gl.TEXTURE_2D_ARRAY,10,gl.RGBA8,512,512,34);
 const scales=new Float32Array(68);scales.fill(1);const tints=new Float32Array(102);tints.fill(1);
 for(let id=0;id<34;id++){const record=manifest.materials[id]||manifest.materials[0],c=record.color||[158,144,122];let image;
  if(record.texture)image=await createImageBitmap(await (await response(record.texture)).blob());
  else{const bytes=new Uint8Array(512*512*4);for(let i=0;i<bytes.length;i+=4)bytes.set([...c,255],i);image=new ImageData(new Uint8ClampedArray(bytes.buffer),512,512);}
  gl.bindTexture(gl.TEXTURE_2D_ARRAY,texture);gl.texSubImage3D(gl.TEXTURE_2D_ARRAY,0,0,0,id,512,512,1,gl.RGBA,gl.UNSIGNED_BYTE,image);if(image.close)image.close();
  scales.set(record.repeatMetres||[1,1],id*2);tints.set(record.tint||[1,1,1],id*3);
 }
 gl.generateMipmap(gl.TEXTURE_2D_ARRAY);gl.texParameteri(gl.TEXTURE_2D_ARRAY,gl.TEXTURE_MIN_FILTER,gl.LINEAR_MIPMAP_LINEAR);gl.texParameteri(gl.TEXTURE_2D_ARRAY,gl.TEXTURE_MAG_FILTER,gl.NEAREST);
 gl.texParameteri(gl.TEXTURE_2D_ARRAY,gl.TEXTURE_WRAP_S,gl.REPEAT);gl.texParameteri(gl.TEXTURE_2D_ARRAY,gl.TEXTURE_WRAP_T,gl.REPEAT);
 gl.uniform2fv(gl.getUniformLocation(program,'repeatSize[0]'),scales);gl.uniform3fv(gl.getUniformLocation(program,'materialTint[0]'),tints);return texture;
}
function uploadInstances(chunk,buffer){
 const vao=gl.createVertexArray();gl.bindVertexArray(vao);const vb=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,vb);gl.bufferData(gl.ARRAY_BUFFER,buffer,gl.STATIC_DRAW);
 for(let k=0;k<4;k++)for(const [at,size,offset] of [[5+k,3,k*12],[9+k,2,48+k*8]]){gl.enableVertexAttribArray(at);gl.vertexAttribPointer(at,size,gl.FLOAT,false,88,offset);gl.vertexAttribDivisor(at,1);}
 gl.enableVertexAttribArray(13);gl.vertexAttribPointer(13,1,gl.FLOAT,false,88,84);gl.vertexAttribDivisor(13,1);
 const centre=mul(add(chunk.bounds[0],chunk.bounds[1]),.5),radius=length(sub(chunk.bounds[1],centre));
 const instanceRecords=chunk.category<3?new Float32Array(chunk.instanceCount*12):null;
 if(instanceRecords){const raw=new Float32Array(buffer);for(let i=0;i<chunk.instanceCount;i++)instanceRecords.set(raw.subarray(i*22,i*22+12),i*12);}
 chunks.push({...chunk,vao,centre,radius,instanceRecords});
}
function upload(chunk,buffer){if(chunk.instanceCount){uploadInstances(chunk,buffer);return;}const vao=gl.createVertexArray();gl.bindVertexArray(vao);const vb=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,vb);gl.bufferData(gl.ARRAY_BUFFER,new Uint8Array(buffer,chunk.offset,chunk.vertexBytes),gl.STATIC_DRAW);
 for(const [at,size,type,normal,offset] of [[0,3,gl.FLOAT,false,0],[1,2,gl.FLOAT,false,12],[2,3,gl.BYTE,true,20],[3,1,gl.UNSIGNED_BYTE,false,23],[4,3,gl.UNSIGNED_BYTE,true,24]]){gl.enableVertexAttribArray(at);gl.vertexAttribPointer(at,size,type,normal,manifest.stride,offset);}
 const ib=gl.createBuffer();gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER,ib);gl.bufferData(gl.ELEMENT_ARRAY_BUFFER,new Uint8Array(buffer,chunk.offset+chunk.vertexBytes,chunk.indexBytes),gl.STATIC_DRAW);
 // Interior camera probes need positions only, not retained UVs, normals or
 // ground buffers. Copy compact coordinates so the upload buffer can be freed.
 const raw=new Float32Array(buffer,chunk.offset,chunk.vertexBytes/4),count=chunk.vertexBytes/manifest.stride;
 const positions=chunk.category===0 || chunk.category===1 || chunk.category===2 ? new Float32Array(count*3) : null;
 if(positions)for(let i=0;i<count;i++)positions.set(raw.subarray(i*7,i*7+3),i*3);
 const indicesData=positions ? new Uint32Array(new Uint32Array(buffer,chunk.offset+chunk.vertexBytes,chunk.indices)) : null;
 const centre=mul(add(chunk.bounds[0],chunk.bounds[1]),.5),radius=length(sub(chunk.bounds[1],centre));chunks.push({...chunk,vao,centre,radius,positions,indicesData});
}
function overview(){const a=manifest.bounds[0],b=manifest.bounds[1],c=mul(add(a,b),.5),span=Math.max(b[0]-a[0],b[2]-a[2]);lookAt([c[0]+span*.48,span*.85,c[2]+span*.75],[c[0],0,c[2]]);$('cutaway').checked=false;}
function street(){let chosen=null,score=-Infinity;for(const route of manifest.routes)for(let i=1;i<route.path.length;i++){const a=route.path[i-1],b=route.path[i],d=length(sub(a,b));if(d<18||route.width<2.5)continue;const mid=mul(add(a,b),.5);const nearby=manifest.buildings.reduce((n,h)=>n+(h.rooms>0 && length(sub(h.footprint[0],mid))<35 ? (h.jettyMetres?.some(v=>v>.1)?8:h.wallHeight>4?3:.1):0),0);const s=nearby-Math.hypot(mid[0],mid[2])*.01;if(s>score){chosen=[a,b];score=s;}}
 if(chosen){const [a,b]=chosen,eye=add(add(a,mul(sub(b,a),.25)),[0,1.65,0]);lookAt(eye,add(b,[0,2.3,0]));}else visit(false);
 $('cutaway').checked=false;
}
function updateBuilding(){const b=manifest.buildings[activeBuilding];inspector?.update(b);if(!b){$('buildingInfo').textContent='Terrain, terraces and stairways';$('interior').disabled=true;$('nextRoom').disabled=true;$('landmark').disabled=true;return;}$('buildingInfo').textContent=`${b.kind.replaceAll('-',' ')} · ${b.rooms||0} rooms · ${b.windows||0} windows`;$('interior').disabled=!b.interiorViews.length;$('nextRoom').disabled=b.interiorViews.length<2;}
// Choose a useful initial look direction from the actual room surfaces.
// This is camera placement, not a replacement interior or collision mesh.
function interiorHeading(eye){const segments=[];
 for(const chunk of chunks){if(!chunk.positions&&!chunk.instanceRecords)continue;const [low,high]=chunk.bounds;if(eye[1]<low[1]||eye[1]>high[1]||eye[0]<low[0]-20||eye[0]>high[0]+20||eye[2]<low[2]-20||eye[2]>high[2]+20)continue;
  const p=chunk.instanceRecords||chunk.positions,indices=chunk.indicesData;
  const total=chunk.instanceCount?chunk.instanceCount*6:indices.length;
  const address=i=>chunk.instanceCount?Math.floor(i/6)*12+QUAD_CORNERS[i%6]*3:indices[i]*3;
  for(let i=0;i<total;i+=3){const hits=[];
   for(let k=0;k<3;k++){const a=address(i+k),b=address(i+(k+1)%3),ay=p[a+1],by=p[b+1];if((ay>eye[1])===(by>eye[1]))continue;const t=(eye[1]-ay)/(by-ay);hits.push([p[a]+(p[b]-p[a])*t-eye[0],p[a+2]+(p[b+2]-p[a+2])*t-eye[2]]);}
   if(hits.length===2)segments.push([hits[0][0],hits[0][1],hits[1][0]-hits[0][0],hits[1][1]-hits[0][1]]);
  }
 }
 const distances=[];
 for(let k=0;k<48;k++){const angle=k*Math.PI*2/48,dx=Math.cos(angle),dz=Math.sin(angle);let nearest=20;
  for(const [ax,az,ex,ez] of segments){const den=dx*ez-dz*ex;if(Math.abs(den)<1e-8)continue;const t=(ax*ez-az*ex)/den,u=(ax*dz-az*dx)/den;if(t>.04&&u>=0&&u<=1)nearest=Math.min(nearest,t);}
  distances.push(nearest);
 }
 let best=0,score=-1;for(let k=0;k<48;k++){const open=distances[k]*.4+Math.min(distances[(k+44)%48],distances[(k+4)%48])*.6;if(open>score){score=open;best=k;}}
 const angle=best*Math.PI*2/48;return [Math.cos(angle),-.16,Math.sin(angle)];
}
function visit(inside){const b=manifest.buildings[activeBuilding];if(!b)return;const centre=mul(b.footprint.reduce((a,p)=>add(a,p),[0,0,0]),1/b.footprint.length);
 if(inside && b.interiorViews.length){const eye=b.interiorViews[state.room%b.interiorViews.length];lookAt(eye,add(eye,interiorHeading(eye)));state.speed=3;$('speed').value=3;$('speedLabel').textContent='3 m/s';}
 else if(b.exteriorDoors.length){const door=b.exteriorDoors[0],dir=normalize([door[0]-centre[0],0,door[2]-centre[2]]);const out=length(dir)>.1?dir:[0,0,1];lookAt(add(add(door,mul(out,6)),[0,1.65,0]),add(centre,[0,2.2,0]));}
 else{const radius=Math.max(...b.footprint.map(p=>Math.hypot(p[0]-centre[0],p[2]-centre[2])));lookAt(add(add(centre,mul(normalize([.6,0,1]),radius+10)),[0,Math.max(6,b.wallHeight*.85),0]),add(centre,[0,b.wallHeight*.45,0]));}
 $('cutaway').checked=false;updateBuilding();}
function selectBuilding(index,travel=false){if(!manifest.buildings[index])return;activeBuilding=index;state.room=0;$('landmark').value=index;updateBuilding();if(travel)visit(false);}
function insideXZ(p,ring){let inside=false;for(let i=0,j=ring.length-1;i<ring.length;j=i++)if((ring[i][2]>p[2])!==(ring[j][2]>p[2])&&p[0]<(ring[j][0]-ring[i][0])*(p[2]-ring[i][2])/(ring[j][2]-ring[i][2])+ring[i][0])inside=!inside;return inside;}
function enterBlueprintRoom(room){const ring=room.boundary;if(!ring?.length)return;
 const xs=ring.map(p=>p[0]),zs=ring.map(p=>p[2]),x0=Math.min(...xs),x1=Math.max(...xs),z0=Math.min(...zs),z1=Math.max(...zs);let eye=null,best=-1;
 for(let i=1;i<12;i++)for(let j=1;j<12;j++){const p=[x0+(x1-x0)*i/12,ring[0][1]+1.65,z0+(z1-z0)*j/12];if(!insideXZ(p,ring))continue;let clearance=Infinity;
  for(let k=0;k<ring.length;k++){const a=ring[k],b=ring[(k+1)%ring.length],dx=b[0]-a[0],dz=b[2]-a[2],t=Math.max(0,Math.min(1,((p[0]-a[0])*dx+(p[2]-a[2])*dz)/(dx*dx+dz*dz||1)));clearance=Math.min(clearance,Math.hypot(p[0]-a[0]-t*dx,p[2]-a[2]-t*dz));}
  if(clearance>best){best=clearance;eye=p;}}
 if(!eye)return;lookAt(eye,add(eye,interiorHeading(eye)));$('cutaway').checked=false;state.speed=3;$('speed').value=3;$('speedLabel').textContent='3 m/s';
}
function rayBox(origin,direction,low,high){let near=0,far=Infinity;for(let k=0;k<3;k++){if(Math.abs(direction[k])<1e-9){if(origin[k]<low[k]||origin[k]>high[k])return Infinity;continue;}let a=(low[k]-origin[k])/direction[k],b=(high[k]-origin[k])/direction[k];if(a>b)[a,b]=[b,a];near=Math.max(near,a);far=Math.min(far,b);if(near>far)return Infinity;}return near;}
function pickBuilding(clientX,clientY){if(!state.ready)return;const rect=canvas.getBoundingClientRect(),x=(clientX-rect.left)/rect.width*2-1,y=1-(clientY-rect.top)/rect.height*2;
 const f=forward(),right=normalize(cross(f,[0,1,0])),up=cross(right,f),tan=Math.tan(Math.PI*68/360),direction=normalize(add(f,add(mul(right,x*tan*rect.width/rect.height),mul(up,y*tan))));
 const candidates=[];manifest.buildings.forEach((b,i)=>{if(!b.footprint.length)return;const xs=b.footprint.map(p=>p[0]),zs=b.footprint.map(p=>p[2]);const near=rayBox(state.position,direction,[Math.min(...xs)-1,b.base-.3,Math.min(...zs)-1],[Math.max(...xs)+1,b.base+b.wallHeight+b.roofHeight+1,Math.max(...zs)+1]);if(Number.isFinite(near))candidates.push({b,i});});if(!candidates.length)return;
 let nearest=Infinity,hit=null;
 for(const chunk of chunks){const near=rayBox(state.position,direction,...chunk.bounds);if((!chunk.positions&&!chunk.instanceRecords) || (chunk.category===1&&!$('roofs').checked) || !Number.isFinite(near) || near>nearest)continue;
  const p=chunk.instanceRecords||chunk.positions,idx=chunk.indicesData;
  const total=chunk.instanceCount?chunk.instanceCount*6:idx.length;
  const address=i=>chunk.instanceCount?Math.floor(i/6)*12+QUAD_CORNERS[i%6]*3:idx[i]*3;
  for(let i=0;i<total;i+=3){const a=address(i),b=address(i+1),c=address(i+2),ax=p[a],ay=p[a+1],az=p[a+2];
   const e1=[p[b]-ax,p[b+1]-ay,p[b+2]-az],e2=[p[c]-ax,p[c+1]-ay,p[c+2]-az],h=cross(direction,e2),det=dot(e1,h);if(Math.abs(det)<1e-9)continue;
   const delta=[state.position[0]-ax,state.position[1]-ay,state.position[2]-az],u=dot(delta,h)/det;if(u<0||u>1)continue;const q=cross(delta,e1),v=dot(direction,q)/det;if(v<0||u+v>1)continue;const t=dot(e2,q)/det;if(t<.01||t>=nearest)continue;
   const point=add(state.position,mul(direction,t));if($('cutaway').checked&&point[1]>Number($('cutHeight').value))continue;nearest=t;hit=point;
  }
 }
 if(!hit)return;let selected=null,gap=1.05;
 for(const {b,i} of candidates){if(hit[1]<b.base-.25||hit[1]>b.base+b.wallHeight+b.roofHeight+1)continue;let d=insideXZ(hit,b.footprint)?0:Infinity;
  for(let k=0;k<b.footprint.length;k++){const a=b.footprint[k],c=b.footprint[(k+1)%b.footprint.length],dx=c[0]-a[0],dz=c[2]-a[2],t=Math.max(0,Math.min(1,((hit[0]-a[0])*dx+(hit[2]-a[2])*dz)/(dx*dx+dz*dz||1)));d=Math.min(d,Math.hypot(hit[0]-a[0]-t*dx,hit[2]-a[2]-t*dz));}if(d<gap){gap=d;selected=i;}}
 if(selected!==null)selectBuilding(selected);
}
function mapTransform(){const a=manifest.bounds[0],b=manifest.bounds[1],scale=205/Math.max(b[0]-a[0],b[2]-a[2]);return {scale,x:115-(a[0]+b[0])*.5*scale,z:115-(a[2]+b[2])*.5*scale};}
function drawMap(){const c=$('map').getContext('2d'),m=mapTransform();c.fillStyle='#20302c';c.fillRect(0,0,230,230);c.strokeStyle='#566553';c.lineWidth=1;
 for(const r of manifest.routes){c.beginPath();r.path.forEach((p,i)=>c[i?'lineTo':'moveTo'](m.x+p[0]*m.scale,m.z+p[2]*m.scale));c.stroke();}
 c.fillStyle='#b39975';for(const b of manifest.buildings){c.fillStyle=b===manifest.buildings[activeBuilding]?'#f4ce78':'#b39975';c.beginPath();b.footprint.forEach((p,i)=>c[i?'lineTo':'moveTo'](m.x+p[0]*m.scale,m.z+p[2]*m.scale));c.closePath();c.fill();}
 const x=m.x+state.position[0]*m.scale,z=m.z+state.position[2]*m.scale;c.strokeStyle='#e5f3df';c.fillStyle='#e5f3df';c.beginPath();c.arc(x,z,3,0,Math.PI*2);c.fill();c.beginPath();c.moveTo(x,z);c.lineTo(x+Math.sin(state.yaw)*14,z-Math.cos(state.yaw)*14);c.stroke();
}
function inFrustum(chunk,m){for(const [row,sign] of [[0,1],[0,-1],[1,1],[1,-1],[2,1],[2,-1]]){const a=m[3]+sign*m[row],b=m[7]+sign*m[4+row],c=m[11]+sign*m[8+row],d=m[15]+sign*m[12+row];if(a*chunk.centre[0]+b*chunk.centre[1]+c*chunk.centre[2]+d < -chunk.radius*Math.hypot(a,b,c))return false;}return true;}
let last=performance.now(),fpsTime=last;
function frame(now){const dt=Math.min((now-last)/1000,.05);last=now;requestAnimationFrame(frame);if(!state.ready)return;
 const ratio=Math.min(devicePixelRatio||1,1.25),w=Math.round(innerWidth*ratio),h=Math.round(innerHeight*ratio);if(canvas.width!==w||canvas.height!==h){canvas.width=w;canvas.height=h;gl.viewport(0,0,w,h);}
 const f=forward(),right=[Math.cos(state.yaw),0,Math.sin(state.yaw)],keys=state.keys;let move=[0,0,0];if(keys.has('KeyW')||keys.has('ArrowUp'))move=add(move,f);if(keys.has('KeyS')||keys.has('ArrowDown'))move=sub(move,f);if(keys.has('KeyD'))move=add(move,right);if(keys.has('KeyA'))move=sub(move,right);if(keys.has('KeyE')||keys.has('Space'))move[1]++;if(keys.has('KeyQ')||keys.has('KeyC'))move[1]--;if(keys.has('ArrowLeft'))state.yaw-=dt;if(keys.has('ArrowRight'))state.yaw+=dt;
 if(length(move))state.position=add(state.position,mul(normalize(move),dt*state.speed*(keys.has('ShiftLeft')||keys.has('ShiftRight')?4:1)));
 gl.clearColor(.64,.72,.73,1);gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);gl.useProgram(program);projectionView=cameraMatrix();gl.uniformMatrix4fv(gl.getUniformLocation(program,'matrix'),false,projectionView);gl.uniform3fv(gl.getUniformLocation(program,'eye'),state.position);
 gl.uniform1f(gl.getUniformLocation(program,'farDistance'),manifest.generation.mode==='world-overview'?40000:4000);gl.uniform1f(gl.getUniformLocation(program,'fogDistance'),manifest.generation.mode==='world-overview'?40000:1800);
 gl.uniform1i(gl.getUniformLocation(program,'textured'),$('textures').checked);gl.uniform1f(gl.getUniformLocation(program,'cutHeight'),$('cutaway').checked?Number($('cutHeight').value):100000);gl.activeTexture(gl.TEXTURE0);gl.bindTexture(gl.TEXTURE_2D_ARRAY,textures);
 let visible=0,draws=0;for(const chunk of chunks){if(chunk.category===1&&!$('roofs').checked)continue;if(!inFrustum(chunk,projectionView))continue;gl.bindVertexArray(chunk.vao);gl.uniform1i(instanceUniform,!!chunk.instanceCount);if(chunk.instanceCount)gl.drawArraysInstanced(gl.TRIANGLES,0,6,chunk.instanceCount);else gl.drawElements(gl.TRIANGLES,chunk.indices,gl.UNSIGNED_INT,0);visible+=chunk.instanceCount?chunk.instanceCount*2:chunk.indices/3;draws++;}
 state.frames++;if(now-fpsTime>500){state.fps=Math.round(state.frames*1000/(now-fpsTime));state.frames=0;fpsTime=now;$('stats').textContent=`${state.fps} FPS · ${(visible/1e6).toFixed(2)}M triangles · ${state.position[1].toFixed(1)} m`;drawMap();}
 // Read-only diagnostics also make browser smoke checks reproducible.
 window.cityViewer={ready:true,fps:state.fps,draws,visibleTriangles:visible,loadedTriangles:manifest.triangles,instancedPanels:manifest.instancedPanels||0,geometryBytes:manifest.bytes,buildings:manifest.buildings.length,position:[...state.position],generation:manifest.generation,glError:gl.getError(),selectedBuildingId:manifest.buildings[activeBuilding]?.id,selectedRoomId:inspector?.room?.id,inspectCamera:(eye,target)=>{
  if(eye.length!==3||target.length!==3||![...eye,...target].every(Number.isFinite)||length(sub(eye,target))<.01)throw Error('Invalid inspection camera');
  lookAt(eye,target);
 }};
}
function bind(){const focusUI=e=>!!e.target.closest('input,select,textarea,button,[role=button],summary');
 window.addEventListener('keydown',e=>{if(focusUI(e))return;if(['Space','ArrowUp','ArrowDown','ArrowLeft','ArrowRight'].includes(e.code))e.preventDefault();state.keys.add(e.code);if(e.repeat)return;if(e.code==='KeyO')overview();if(e.code==='KeyG')street();});window.addEventListener('keyup',e=>state.keys.delete(e.code));window.addEventListener('blur',()=>state.keys.clear());
 let pointerStart=null,pointerMoved=false;
 canvas.addEventListener('pointerdown',e=>{pointerStart=[e.clientX,e.clientY];pointerMoved=false;state.drag=true;canvas.focus();canvas.setPointerCapture(e.pointerId);});
 canvas.addEventListener('pointermove',e=>{if(pointerStart&&Math.hypot(e.clientX-pointerStart[0],e.clientY-pointerStart[1])>5)pointerMoved=true;});
 canvas.addEventListener('pointerup',e=>{state.drag=false;if(!pointerMoved&&pointerStart&&document.pointerLockElement!==canvas)pickBuilding(e.clientX,e.clientY);pointerStart=null;});canvas.addEventListener('pointercancel',()=>{state.drag=false;pointerStart=null;});
 document.addEventListener('mousemove',e=>{if(document.pointerLockElement===canvas||state.drag){state.yaw+=e.movementX*.0025;state.pitch=Math.max(-1.5,Math.min(1.5,state.pitch-e.movementY*.0025));}});
 document.addEventListener('pointerlockchange',()=>{document.body.classList.toggle('flying',document.pointerLockElement===canvas);state.drag=false;state.keys.clear();});
 $('fly').onclick=async()=>{canvas.focus();try{await canvas.requestPointerLock();}catch{$('summary').textContent='Mouse lock unavailable here; drag the canvas to look around.';}};
 $('overview').onclick=overview;$('street').onclick=street;$('visit').onclick=()=>visit(false);$('interior').onclick=()=>{state.room=0;visit(true);};$('nextRoom').onclick=()=>{state.room++;visit(true);};
 $('landmark').onchange=()=>selectBuilding(Number($('landmark').value));
 $('speed').oninput=()=>{state.speed=Number($('speed').value);$('speedLabel').textContent=`${state.speed} m/s`;};$('cutHeight').oninput=()=>{$('cutLabel').textContent=`${$('cutHeight').value} m`;};
 canvas.addEventListener('wheel',e=>{e.preventDefault();state.speed=Math.max(1,Math.min(80,state.speed*Math.exp(-e.deltaY*.001)));$('speed').value=state.speed;$('speedLabel').textContent=`${state.speed.toFixed(0)} m/s`;},{passive:false});
 $('map').onclick=e=>{const rect=$('map').getBoundingClientRect(),m=mapTransform();state.position[0]=((e.clientX-rect.left)*230/rect.width-m.x)/m.scale;state.position[2]=((e.clientY-rect.top)*230/rect.height-m.z)/m.scale;};
}
try{if(!gl)throw Error('This browser does not support WebGL2. Open this page in Chrome or Safari with hardware acceleration enabled.');
 program=makeProgram();instanceUniform=gl.getUniformLocation(program,'instanced');gl.useProgram(program);gl.uniform1i(gl.getUniformLocation(program,'groundMask'),1);gl.enable(gl.DEPTH_TEST);gl.disable(gl.CULL_FACE);
 manifest=await (await response('city.json')).json();if(manifest.schema!==1||manifest.stride!==28)throw Error('Unsupported city mesh format');
 $('summary').textContent=`Seed ${manifest.generation.seed??'native'} · ${manifest.buildings.length.toLocaleString()} buildings · ${(manifest.structure?.rooms??manifest.buildings.reduce((n,b)=>n+(b.rooms||0),0)).toLocaleString()} rooms`;
 $('origin').textContent=`${manifest.sourcePipeline}. World site ${manifest.generation.siteCell??'selected'}; only its city region is loaded. All generated interiors are included.`;
 if(manifest.sourcePipeline==='native-district'){document.querySelector('h1').textContent='The terraced merchant quarter.';$('origin').textContent='Native streets, stairs, inns, pubs and dwellings. Choose a building to inspect its interior.';}
 if(manifest.generation.mode==='base-first-mountain-city'){document.querySelector('h1').textContent='The hillside city.';$('origin').textContent='Cliff terraces, stone alleys and switchback stairs. Explore the inns, pubs and homes above the hillside.';}
 if(manifest.sourcePipeline==='native-compound'){document.querySelector('h1').textContent=manifest.buildings.some(b=>b.kind==='defence-tower')?'Inside the Gothic citadel.':'Explore the garden estate.';$('origin').textContent='Generated grounds and connected wings. Choose a building to inspect its interior.';}
 if(manifest.generation.mode==='world-overview'){
  document.querySelector('h1').textContent='The generated world.';$('summary').textContent=`Seed ${manifest.generation.seed} · ${manifest.generation.extentMetres/1000} km across · native terrain`;
  $('origin').textContent='The same native terrain field used to place the historical settlements.';
  const link=document.createElement('a');link.href=manifest.generation.cityUrl;link.textContent='Enter the capital →';link.style.cssText='display:block;padding:12px;color:#efce90';$('regionNav').append(link);
  $('speed').max=1500;$('speed').value=600;state.speed=600;$('speedLabel').textContent='600 m/s';
 }
 $('notes').textContent=`${manifest.triangles.toLocaleString()} triangles. ${manifest.rejections.length} native generation notes. Free flight has no collision restriction.`;
 if(manifest.generation.layout==='districts')document.querySelector('h1').textContent=manifest.buildings.length?'The raised district city.':'Raised districts.';
 if(manifest.generation.layout==='voronoi')document.querySelector('h1').textContent=manifest.buildings.length?'The Voronoi terrace city.':'Voronoi landforms.';
 $('landmark').replaceChildren();const sorted=manifest.buildings.map((b,i)=>[b,i]).sort((a,b)=>(a[0].kind==='house')-(b[0].kind==='house')||a[0].kind.localeCompare(b[0].kind));for(const [b,i] of sorted){const option=document.createElement('option');option.value=i;option.textContent=`${b.kind.replaceAll('-',' ')} ${i+1} · ${b.rooms||0} rooms`;$('landmark').append(option);}activeBuilding=sorted.find(([b])=>b.history?.residents?.length&&b.roomList?.some(r=>r.boundary?.length))?.[1]??sorted.find(([b])=>b.interiorViews.length)?.[1]??0;$('landmark').value=activeBuilding;inspector=createInspector(manifest,{select:selectBuilding,enterRoom:enterBlueprintRoom});updateBuilding();
 $('loadingText').textContent='Loading stone, timber, roofs and ground';textures=await loadTextures();
 if(manifest.groundMask){
 const mask=gl.createTexture();gl.activeTexture(gl.TEXTURE1);gl.bindTexture(gl.TEXTURE_2D,mask);
 const bitmap=await createImageBitmap(await(await response(manifest.groundMask.texture)).blob());
 gl.texImage2D(gl.TEXTURE_2D,0,gl.RGB,gl.RGB,gl.UNSIGNED_BYTE,bitmap);
 gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);
 gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);
 gl.uniform1i(gl.getUniformLocation(program,'groundMask'),1);gl.uniform1i(gl.getUniformLocation(program,'hasGroundMask'),1);
 gl.uniform4fv(gl.getUniformLocation(program,'groundMaskBounds'),manifest.groundMask.bounds);gl.activeTexture(gl.TEXTURE0);
 }
$('progress').value=10;
 $('loadingText').textContent=`Loading city geometry · ${(manifest.bytes/1e6).toFixed(0)} MB`;
 // Stream one packed chunk at a time; never retain a second full city in JS.
 const r=await response('city.bin'),reader=r.body.getReader();let pending=new Uint8Array(0),used=0,total=0;
 for(let i=0;i<manifest.chunks.length;i++){
  const chunk=manifest.chunks[i],buffer=new Uint8Array(chunk.vertexBytes+chunk.indexBytes+(chunk.instanceBytes||0));let filled=0;
  if(chunk.offset!==total)throw Error('Non-contiguous city mesh chunk');
  while(filled<buffer.length){
   if(used===pending.length){const part=await reader.read();if(part.done)throw Error('City mesh download was incomplete');pending=part.value;used=0;}
   const count=Math.min(buffer.length-filled,pending.length-used);buffer.set(pending.subarray(used,used+count),filled);used+=count;filled+=count;
  }
  upload({...chunk,offset:0},buffer.buffer);total+=buffer.length;
  $('progress').value=10+90*total/manifest.bytes;
  if(i%20===0){$('loadingText').textContent=`Preparing city · ${i}/${manifest.chunks.length}`;await new Promise(requestAnimationFrame);}
 }
 if(total!==manifest.bytes || used!==pending.length || !(await reader.read()).done)throw Error('City mesh size mismatch');
 reader.releaseLock();pending=null;
 const error=gl.getError();if(error!==gl.NO_ERROR)throw Error(`GPU upload failed (${error})`);bind();overview();state.ready=true;last=fpsTime=performance.now();$('loading').hidden=true;requestAnimationFrame(frame);
}catch(error){console.error(error);$('loading').hidden=false;$('loadingText').textContent=error.message;$('progress').hidden=true;window.cityViewer={ready:false,error:error.message};}
canvas.addEventListener('webglcontextlost',e=>{e.preventDefault();state.ready=false;$('loading').hidden=false;$('loadingText').textContent='Graphics context lost. Reload to restore the city.';});
