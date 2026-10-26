// Native GPU acceptance capture of the actual rewrite CDT mesh. No heightfield
// reconstruction, GUI automation, or vertical exaggeration. Fixed 1x cameras.
import Foundation
import Metal
import simd
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8)); exit(1)
}
guard CommandLine.arguments.count == 4 else { fail("usage: TerrainRewriteNativeCapture product.json output.png top|oblique") }
let input = CommandLine.arguments[1], output = CommandLine.arguments[2], view = CommandLine.arguments[3]
guard view == "top" || view == "oblique" else { fail("unknown fixed camera") }
let data = try Data(contentsOf: URL(fileURLWithPath: input))
guard let report = try JSONSerialization.jsonObject(with: data) as? [String:Any],
      let mesh = report["mesh"] as? [String:Any], let positions = mesh["vertices"] as? [[Double]],
      let triangles = mesh["triangles"] as? [[Int]], !positions.isEmpty, !triangles.isEmpty else { fail("invalid mesh product") }
for p in positions { guard p.count == 3 && p.allSatisfy({$0.isFinite}) else { fail("invalid vertex") } }
for t in triangles { guard t.count == 3 && t.allSatisfy({$0 >= 0 && $0 < positions.count}) else { fail("invalid triangle") } }
guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { fail("Metal device unavailable") }
struct Vertex { var position: SIMD4<Float>; var normal: SIMD4<Float>; var coverStyle: SIMD4<Float> }
struct Uniforms { var rowX: SIMD4<Float>; var rowY: SIMD4<Float>; var rowZ: SIMD4<Float>; var material: SIMD4<Float> }
func point(_ i: Int) -> SIMD3<Float> { let p=positions[i]; return SIMD3(Float(p[0]),Float(p[1]),Float(p[2])) }
var vertices = [Vertex](); vertices.reserveCapacity(triangles.count * 3)
let roles = mesh["roles"] as? [Int] ?? [Int](repeating:0,count:triangles.count)
guard roles.count == triangles.count && roles.allSatisfy({$0==0 || $0==1 || $0==2}) else { fail("invalid material roles") }
let materialIDs = mesh["materials"] as? [Int]
if [10,11,15,16,17].contains(report["phase"] as? Int ?? 0) {guard let ids=materialIDs,ids.count==triangles.count,ids.allSatisfy({$0>=0 && $0<=([11,15,16,17].contains(report["phase"] as? Int ?? 0) ? 13 : 10)}) else {fail("invalid arrangement material IDs")}}
let coverStyles=mesh["coverStyles"] as? [[Double]]
if let styles=coverStyles {guard styles.count==triangles.count && styles.allSatisfy({$0.count==4 && $0.allSatisfy({$0.isFinite}) && abs($0[3])<=16}) else {fail("invalid triangle cover styles")}}
let expectedNormals = mesh["expectedNormals"] as? [[Double]]
if let normals = expectedNormals { guard normals.count == triangles.count else { fail("normal count mismatch") } }
for (triangleIndex,t) in triangles.enumerated() {
    let a=point(t[0]), b=point(t[1]), c=point(t[2]); let raw=simd_cross(b-a,c-a)
    guard simd_length(raw) > 1e-8 else { fail("degenerate packed native triangle") }
    let n=simd_normalize(raw)
    if let normals = expectedNormals, normals[triangleIndex].count == 3, abs(normals[triangleIndex][1]) < 1e-7 {
        let e=normals[triangleIndex]; let expected=SIMD3<Float>(Float(e[0]),0,Float(e[2]))
        guard abs(n.y)<1e-5 && abs(simd_length(expected)-1)<1e-4 && simd_dot(n,expected) > 0.999 else { fail("vertical face winding/normal mismatch") }
    } else if [10,15,16,17].contains(report["phase"] as? Int ?? 0), let normals=expectedNormals, normals[triangleIndex].count==3, normals[triangleIndex][1]<0 {
        let e=normals[triangleIndex];let expected=SIMD3<Float>(Float(e[0]),Float(e[1]),Float(e[2]))
        guard abs(simd_length(expected)-1)<1e-4 && simd_dot(n,expected)>0.999 else {fail("bridge underside normal mismatch")}
    } else { guard n.y > 0 else { fail("native triangle is not upward wound") } }
    let material=[10,11,15,16,17].contains(report["phase"] as? Int ?? 0) ? 10+materialIDs![triangleIndex] : roles[triangleIndex]
    let style=coverStyles?[triangleIndex] ?? [0,0,0,0]
    for p in [a,b,c] { vertices.append(Vertex(position:SIMD4(p.x,p.y,p.z,1),normal:SIMD4(n.x,n.y,n.z,Float(material)),coverStyle:SIMD4(Float(style[0]),Float(style[1]),Float(style[2]),Float(style[3])))) }
}
let minX=positions.map{$0[0]}.min()!, maxX=positions.map{$0[0]}.max()!
let minZ=positions.map{$0[2]}.min()!, maxZ=positions.map{$0[2]}.max()!
let maxHeight=positions.map{$0[1]}.max()!
let extent=Float(max(maxX-minX,maxZ-minZ)); guard extent > 0 else { fail("invalid view extent") }
let width=view == "top" ? 2048 : 1920, height=view == "top" ? 2048 : 1080
var target=SIMD3<Float>(Float((minX+maxX)/2),0,Float((minZ+maxZ)/2))
var forward=view == "top" ? SIMD3<Float>(0,-1,0) : simd_normalize(SIMD3<Float>(-0.9,(report["phase"] as? Int)==3 ? -2.0 : ([10,15,16,17].contains(report["phase"] as? Int ?? 0) ? -0.85 : -0.48),[6,10,15,16,17].contains(report["phase"] as? Int ?? 0) ? 1.2 : -1.2))
if let azimuth=report["cameraAzimuthDegrees"] as? Double, view == "oblique" {
    guard azimuth.isFinite,abs(azimuth)<=3600 else {fail("invalid camera azimuth")}
    let angle=Float(azimuth*Double.pi/180)
    forward=simd_normalize(SIMD3<Float>(cos(angle),-0.85,sin(angle)))
}
let right=view == "top" ? SIMD3<Float>(1,0,0) : simd_normalize(simd_cross(forward,SIMD3<Float>(0,1,0)))
let up=view == "top" ? SIMD3<Float>(0,0,1) : simd_cross(right,forward)
var spanX=extent * (view == "top" ? 1.1 : 1.55), spanY=spanX*Float(height)/Float(width)
// Fit the complete projected bounds of narrow/diagonal river fixtures without
// changing vertical scale or weakening coverage/geometry validation.
if [3,9,10,15,16,17].contains(report["phase"] as? Int ?? 0) && view == "oblique" {
    let projected=positions.map { p -> SIMD2<Float> in
        let v=SIMD3<Float>(Float(p[0]),Float(p[1]),Float(p[2]))-target
        return SIMD2(simd_dot(v,right),simd_dot(v,up))
    }
    let loX=projected.map{$0.x}.min()!, hiX=projected.map{$0.x}.max()!
    let loY=projected.map{$0.y}.min()!, hiY=projected.map{$0.y}.max()!
    target += right*((loX+hiX)/2)+up*((loY+hiY)/2)
    spanX=max(hiX-loX,(hiY-loY)*Float(width)/Float(height))*1.18
    spanY=spanX*Float(height)/Float(width)
}
func row(_ v:SIMD3<Float>,_ scale:Float,_ bias:Float=0)->SIMD4<Float>{let r=v*scale;return SIMD4(r.x,r.y,r.z,bias-simd_dot(target,r))}
var uniforms=Uniforms(rowX:row(right,2/spanX),rowY:row(up,2/spanY),rowZ:row(forward,1/(extent*4),0.5),material:SIMD4(Float(max(maxHeight,1)),1,0,0))
// Optional audited world-space cover page. Geometry and depth remain the source mesh.
var coverInfo=SIMD4<Float>(0,0,1,0)
var coverWidth=1,coverHeight=1,coverPixels:[UInt8]=[255,255,255,255]
if let cover=report["coverTexture"] as? [String:Any] {
    guard let w=cover["width"] as? Int,let h=cover["height"] as? Int,w>0,h>0,w<=2048,h<=2048,
          let raw=cover["rgba"] as? [Int],raw.count==w*h*4,raw.allSatisfy({$0>=0 && $0<=255}),
          let x=cover["minX"] as? Double,let z=cover["minZ"] as? Double,let spacing=cover["spacing"] as? Double,
          x.isFinite,z.isFinite,spacing.isFinite,spacing>0 else {fail("invalid cover texture")}
    coverWidth=w;coverHeight=h;coverPixels=raw.map{UInt8($0)};coverInfo=SIMD4(Float(x),Float(z),Float(spacing),Float(65536 | (cover["hardMask"] as? Int ?? 0)))
}
let coverDesc=MTLTextureDescriptor.texture2DDescriptor(pixelFormat:.rgba8Unorm,width:coverWidth,height:coverHeight,mipmapped:false)
coverDesc.storageMode = .shared;coverDesc.usage = .shaderRead
let coverTexture=device.makeTexture(descriptor:coverDesc)!
coverTexture.replace(region:MTLRegionMake2D(0,0,coverWidth,coverHeight),mipmapLevel:0,withBytes:coverPixels,bytesPerRow:coverWidth*4)
let shader="""
#include <metal_stdlib>
using namespace metal;
struct Vertex { float4 position; float4 normal; float4 coverStyle; };
struct Uniforms { float4 rowX; float4 rowY; float4 rowZ; float4 material; };
struct Raster { float4 position [[position]]; float3 normal; float height; float water; float2 worldXZ; float4 coverStyle; };
vertex Raster terrain_vertex(uint id [[vertex_id]],const device Vertex *vertices [[buffer(0)]],constant Uniforms &u [[buffer(1)]]) {
    Vertex v=vertices[id];return {float4(dot(v.position,u.rowX),dot(v.position,u.rowY),dot(v.position,u.rowZ),1),v.normal.xyz,v.position.y,v.normal.w,v.position.xz,v.coverStyle};
}
fragment float4 terrain_fragment(Raster in [[stage_in]],constant Uniforms &u [[buffer(1)]],constant float4 &coverInfo [[buffer(2)]],texture2d<float> covers [[texture(0)]]) {
    float t=clamp(in.height/u.material.x,0.0f,1.0f);
    float3 color=mix(float3(.24,.40,.23),float3(.66,.58,.43),t);
    if(in.water>.5 && in.water<1.5) color=float3(.10,.48,.66);
    if(in.water>1.5) color=float3(.62,.61,.56);
    if(in.water>9.5) {
        int material=int(round(in.water))-10;
        color=float3(.48,.60,.37);
        if(material==1)color=float3(.72,.66,.49);
        if(material==2)color=float3(.61,.55,.40);
        if(material==3)color=float3(.58,.34,.23);
        if(material==4)color=float3(.65,.46,.28);
        if(material==5)color=float3(.43,.48,.31);
        if(material==6)color=float3(.36,.39,.28);
        if(material==7)color=float3(.62,.61,.56);
        if(material==9)color=float3(.10,.48,.66);
        if(material==10)color=float3(.28,.22,.16);
        if(material==11)color=float3(.57,.61,.43);
        if(material==12)color=float3(.77,.71,.55);
        if(material==13)color=float3(.55,.28,.19);
    }
    if(coverInfo.w>0) {
        constexpr sampler coverSampler(coord::normalized,address::clamp_to_edge,filter::nearest);
        float2 uv=((in.worldXZ-coverInfo.xy)/coverInfo.z+0.5f)/float2(covers.get_width(),covers.get_height());
        float4 texel=covers.sample(coverSampler,uv);
        int sampledOwner=int(round(texel.a*255))-1;
        int expectedOwner=int(round(abs(in.coverStyle.w)))-1;
        bool sampledHard=sampledOwner>=0 && sampledOwner<16 && (uint(coverInfo.w)&(1u<<uint(sampledOwner)))!=0;
        bool wrongHard=in.coverStyle.w>0 && sampledOwner!=expectedOwner;
        bool hardBleed=in.coverStyle.w<0 && sampledHard;
        color=(wrongHard||hardBleed)?in.coverStyle.rgb:texel.rgb;
    }
    float light=.30+.70*max(0.0f,dot(normalize(in.normal),normalize(float3(.4,.8,.3))));
    return float4(color*light,1);
}
"""
let library=try device.makeLibrary(source:shader,options:nil)
let pipeline=MTLRenderPipelineDescriptor();pipeline.vertexFunction=library.makeFunction(name:"terrain_vertex");pipeline.fragmentFunction=library.makeFunction(name:"terrain_fragment");pipeline.colorAttachments[0].pixelFormat = .rgba8Unorm;pipeline.depthAttachmentPixelFormat = .depth32Float
let state=try device.makeRenderPipelineState(descriptor:pipeline)
let depthDesc=MTLDepthStencilDescriptor();depthDesc.depthCompareFunction = .less;depthDesc.isDepthWriteEnabled=true
let depthState=device.makeDepthStencilState(descriptor:depthDesc)!
let colorDesc=MTLTextureDescriptor.texture2DDescriptor(pixelFormat:.rgba8Unorm,width:width,height:height,mipmapped:false);colorDesc.usage = .renderTarget;colorDesc.storageMode = .shared
let depthTextureDesc=MTLTextureDescriptor.texture2DDescriptor(pixelFormat:.depth32Float,width:width,height:height,mipmapped:false);depthTextureDesc.usage = .renderTarget;depthTextureDesc.storageMode = .private
guard let color=device.makeTexture(descriptor:colorDesc),let depth=device.makeTexture(descriptor:depthTextureDesc),let buffer=device.makeBuffer(bytes:vertices,length:vertices.count*MemoryLayout<Vertex>.stride,options:.storageModeShared),let command=queue.makeCommandBuffer() else { fail("Metal allocation failed") }
let pass=MTLRenderPassDescriptor();pass.colorAttachments[0].texture=color;pass.colorAttachments[0].loadAction = .clear;pass.colorAttachments[0].storeAction = .store;pass.colorAttachments[0].clearColor=MTLClearColor(red:0.06,green:0.09,blue:0.12,alpha:0)
pass.depthAttachment.texture=depth;pass.depthAttachment.loadAction = .clear;pass.depthAttachment.storeAction = .dontCare;pass.depthAttachment.clearDepth=1
let encoder=command.makeRenderCommandEncoder(descriptor:pass)!;encoder.setRenderPipelineState(state);encoder.setDepthStencilState(depthState);encoder.setCullMode(.none);encoder.setVertexBuffer(buffer,offset:0,index:0);encoder.setVertexBytes(&uniforms,length:MemoryLayout<Uniforms>.stride,index:1);encoder.setFragmentBytes(&uniforms,length:MemoryLayout<Uniforms>.stride,index:1);encoder.setFragmentTexture(coverTexture,index:0);encoder.setFragmentBytes(&coverInfo,length:MemoryLayout<SIMD4<Float>>.stride,index:2);encoder.drawPrimitives(type:.triangle,vertexStart:0,vertexCount:vertices.count);encoder.endEncoding();command.commit();command.waitUntilCompleted()
guard command.status == .completed else { fail("GPU capture failed: \(String(describing:command.error))") }
var pixels=[UInt8](repeating:0,count:width*height*4);color.getBytes(&pixels,bytesPerRow:width*4,from:MTLRegionMake2D(0,0,width,height),mipmapLevel:0)
var covered=0;for i in stride(from:3,to:pixels.count,by:4){if pixels[i] != 0 { covered += 1 }}
// A standalone road ribbon occupies much less area than a terrain tile. Validate
// its coverage against projected triangle area, rather than a terrain occupancy
// threshold. Keep the existing terrain policy unchanged for previous phases.
var expectedRoadPixels:Double = 0
if (report["phase"] as? Int)==9 {
    func screen(_ p:SIMD3<Float>)->SIMD2<Double> {
        let q=p-target
        return SIMD2(Double(simd_dot(q,right)/spanX+0.5)*Double(width), Double(simd_dot(q,up)/spanY+0.5)*Double(height))
    }
    for t in triangles {
        let a=screen(point(t[0])),b=screen(point(t[1])),c=screen(point(t[2]))
        for p in [a,b,c] {guard p.x>0 && p.x<Double(width) && p.y>0 && p.y<Double(height) else {fail("road capture clipped geometry")}}
        expectedRoadPixels += abs((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x))*0.5
    }
    guard expectedRoadPixels>1000 && Double(covered)>expectedRoadPixels*0.95 && Double(covered)<expectedRoadPixels*1.05 else {fail("road capture differs from projected mesh area")}
} else {
    guard covered > width*height/10 && covered < width*height*95/100 else { fail("native capture coverage outside acceptance range") }
}
// Write opaque PNG while preserving raw RGB values and report geometry coverage.
for i in stride(from:3,to:pixels.count,by:4){pixels[i]=255}
let provider=CGDataProvider(data:Data(pixels) as CFData)!
let image=CGImage(width:width,height:height,bitsPerComponent:8,bitsPerPixel:32,bytesPerRow:width*4,space:CGColorSpaceCreateDeviceRGB(),bitmapInfo:CGBitmapInfo(rawValue:CGImageAlphaInfo.last.rawValue),provider:provider,decode:nil,shouldInterpolate:false,intent:.defaultIntent)!
let destination=CGImageDestinationCreateWithURL(URL(fileURLWithPath:output) as CFURL,UTType.png.identifier as CFString,1,nil)!;CGImageDestinationAddImage(destination,image,nil);guard CGImageDestinationFinalize(destination) else { fail("PNG write failed") }
let metadata:[String:Any]=["backend":"Metal", "device":device.name,"camera":view,"width":width,"height":height,"verticalScale":1,"viewBoundsMetres":[minX,minZ,maxX,maxZ],"forward":[forward.x,forward.y,forward.z],"target":[target.x,target.y,target.z],"orthographicSpanMetres":[spanX,spanY],"lightDirection":[0.4,0.8,0.3],"exposure":1,"triangleCount":triangles.count,"coveredPixels":covered,"expectedRoadPixels":expectedRoadPixels,"gpuSeconds":command.gpuEndTime-command.gpuStartTime,"status":"PASS"]
try JSONSerialization.data(withJSONObject:metadata,options:[.prettyPrinted,.sortedKeys]).write(to:URL(fileURLWithPath:output+".json"))
print("PASS native Metal \(view): \(triangles.count) triangles, \(covered) covered pixels on \(device.name)")
