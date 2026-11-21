// SPDX-License-Identifier: MIT
// The requested layout is our declared linear layout, using runtime pitch.
// Existing DVM compressed layouts remain on their original upload path.
#include "MetalSafetyV15.hpp"
@interface RaphaelLinearLayoutV15 : NSObject
+ (instancetype)layoutWithDescriptor:(MTLTextureDescriptor *)d;
- (NSUInteger)size;
- (NSUInteger)row;
@end
@implementation RaphaelLinearLayoutV15 {
 raphael_metal_v15::Linear _geometry;
}
+ (instancetype)layoutWithDescriptor:(MTLTextureDescriptor *)d {
 if(!d || d.textureType!=MTLTextureType2D || d.depth!=1 || d.mipmapLevelCount!=1 ||
    d.arrayLength!=1 || d.sampleCount!=1)return nil;
 uint64_t texel=raphael_metal_v15::texelBytes(d.pixelFormat);
 if(!texel)return nil;
 uint64_t row=nmb_device_linear_bytes_per_row(gDevice,d.pixelFormat,d.width);
 RaphaelLinearLayoutV15 *layout=[self new];
 if(!layout->_geometry.init(d.width,d.height,texel,row,(uint64_t)getpagesize()))return nil;
 return layout;
}
- (NSUInteger)size {return (NSUInteger)_geometry.size;}
- (NSUInteger)row {return (NSUInteger)_geometry.row;}
- (NSUInteger)alignment {return (NSUInteger)_geometry.page;}
- (MTLSize)copyGranularity {return MTLSizeMake(1,1,1);}
- (DVMLayoutWatermark)watermark {DVMLayoutWatermark mark{};return mark;}
- (void)initializeTextureMemory:(void *)memory {(void)memory;}
- (void)finalizeTextureMemory:(void *)memory {(void)memory;}
- (void)copyFromLinearBytes:(const void *)bytes linearOffset:(NSUInteger)offset linearBytesPerRow:(NSUInteger)row
 linearBytesPerImage:(NSUInteger)image toTextureMemory:(void *)memory textureSlice:(NSUInteger)slice
 textureLevel:(NSUInteger)level textureRegion:(MTLRegion)r {
 (void)image;
 if(slice||level||r.origin.z||r.size.depth!=1)return;
 if(!_geometry.copy(memory,(void *)bytes,offset,row,r.origin.x,r.origin.y,r.size.width,r.size.height,true))
  NBLOG(@"native-metal: linear layout upload rejected invalid region or stride");
}
- (void)copyFromTextureMemory:(const void *)memory textureSlice:(NSUInteger)slice textureLevel:(NSUInteger)level
 textureRegion:(MTLRegion)r toLinearBytes:(void *)bytes linearOffset:(NSUInteger)offset
 linearBytesPerRow:(NSUInteger)row linearBytesPerImage:(NSUInteger)image {
 (void)image;
 if(slice||level||r.origin.z||r.size.depth!=1)return;
 if(!_geometry.copy((void *)memory,bytes,offset,row,r.origin.x,r.origin.y,r.size.width,r.size.height,false))
  NBLOG(@"native-metal: linear layout readback rejected invalid region or stride");
}
@end
