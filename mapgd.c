/* $Id$ */

#include "map.h"
#include "mapthread.h"

#include <time.h>

#ifdef USE_ICONV
#include <iconv.h>
#endif

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static void msFixedImageCopy (gdImagePtr dst, gdImagePtr src, int dstX, int dstY, int srcX, int srcY, int w, int h);

static unsigned char PNGsig[8] = {137, 80, 78, 71, 13, 10, 26, 10}; // 89 50 4E 47 0D 0A 1A 0A hex
static unsigned char JPEGsig[3] = {255, 216, 255}; // FF D8 FF hex

int msImageSetPenGD(gdImagePtr img, colorObj *color) 
{
  if(MS_VALID_COLOR(*color))
    color->pen = gdImageColorResolve(img, color->red, color->green, color->blue);
  else
    color->pen = -1;

  return(MS_SUCCESS);
}

int msCompareColors(colorObj *c1, colorObj *c2)
{
  if(c1->red != c2->red || c1->green != c2->green || c1->blue != c2->blue) return MS_FALSE;
  return MS_TRUE;
}

static gdImagePtr searchImageCache(struct imageCacheObj *ic, styleObj *style,
                                   int size) 
{
  struct imageCacheObj *icp;

  icp = ic;
  while(icp) {
    if (icp->symbol == style->symbol
    && msCompareColors(&(icp->color), &(style->color)) == MS_TRUE
    && msCompareColors(&(icp->outlinecolor), &(style->outlinecolor)) == MS_TRUE
    && msCompareColors(&(icp->backgroundcolor), &(style->backgroundcolor)) == MS_TRUE
    && icp->size == size) {
        return(icp->img);
    }
    icp = icp->next;
  }

  return(NULL);
}

static struct imageCacheObj *addImageCache(struct imageCacheObj *ic, int *icsize, styleObj *style, int size, gdImagePtr img) 
{
  struct imageCacheObj *icp;

  if(*icsize > MS_IMAGECACHESIZE) { // remove last element, size stays the same
    icp = ic;
    while(icp->next && icp->next->next) icp = icp->next;
    freeImageCache(icp->next);
    icp->next = NULL;
  } else 
    *icsize += 1;

  if((icp = (struct imageCacheObj *)malloc(sizeof(struct imageCacheObj))) == NULL) {
    msSetError(MS_MEMERR, NULL, "initImageCache()");
    return(NULL);
  }
  
  icp->img = img;
  icp->color = style->color;
  icp->outlinecolor = style->outlinecolor;
  icp->backgroundcolor = style->backgroundcolor;
  icp->symbol = style->symbol;
  icp->size = size;
  icp->next = ic; // insert at the beginning
 
  return(icp);
}

/*
 * Utility function to create a GD image. Returns
 * a pointer to an imageObj structure.
 */  
imageObj *msImageCreateGD(int width, int height, outputFormatObj *format, char *imagepath, char *imageurl) 
{
  imageObj  *image;

  if(width > 0 && height > 0) {
    image = (imageObj *)calloc(1,sizeof(imageObj));

    if(format->imagemode == MS_IMAGEMODE_RGB || format->imagemode == MS_IMAGEMODE_RGBA) {
      image->img.gd = gdImageCreateTrueColor(width, height);
      gdImageAlphaBlending( image->img.gd, 0); // off by default, from alans@wunderground.com
    } else
      image->img.gd = gdImageCreate(width, height);
    
    if(image->img.gd) {
      image->format = format;
      format->refcount++;

      image->width = width;
      image->height = height;
      image->imagepath = NULL;
      image->imageurl = NULL;
            
      if(imagepath) image->imagepath = strdup(imagepath);
      if(imageurl) image->imageurl = strdup(imageurl);
       
      return image;
    } else
      free(image);   
  } else {
    msSetError(MS_IMGERR, "Cannot create GD image of size %d x %d.", "msImageCreateGD()", width, height );
  }

  return NULL;
}

/**
 * Utility function to initialize the color of an image.  The background
 * color is passed, but the outputFormatObj is consulted to see if the
 * transparency should be set (for RGBA images).   Note this function only
 * affects TrueColor images. 
 */  

void msImageInitGD( imageObj *image, colorObj *background )
{
  if(image->format->imagemode == MS_IMAGEMODE_PC256) {
    gdImageColorAllocate(image->img.gd, background->red, background->green, background->blue);
    return;
  }

  {
    int	pen, pixels, line;
    int *tpixels;

    if(image->format->imagemode == MS_IMAGEMODE_RGBA)
      pen = gdTrueColorAlpha(background->red, background->green, background->blue, image->format->transparent ? 127:0);
    else
      pen = gdTrueColor(background->red, background->green, background->blue);

    for(line = 0; line < image->img.gd->sy; line++ ) {
      pixels = image->img.gd->sx;
      tpixels = image->img.gd->tpixels[line];

      while(pixels-- > 0)
        *(tpixels++) = pen;
    }
  }
}

/* msImageLoadGDStream is called by msImageLoadGD and is useful
 * by itself */
imageObj *msImageLoadGDStream(FILE *stream)
{
  gdImagePtr img=NULL;
  const char *driver = NULL;
  char bytes[8];
  imageObj      *image = NULL;

  image = (imageObj *)calloc(1,sizeof(imageObj));
  
  fread(bytes,8,1,stream); // read some bytes to try and identify the file
  rewind(stream); // reset the image for the readers
  if (memcmp(bytes,"GIF8",4)==0) {
#ifdef USE_GD_GIF
    img = gdImageCreateFromGif(stream);
    driver = "GD/GIF";
    image->img.gd = img;
    image->imagepath = NULL;
    image->imageurl = NULL;
    image->width = img->sx;
    image->height = img->sy;
#else
    msSetError(MS_MISCERR, "Unable to load GIF reference image.", "msImageLoadGD()");
    fclose(stream);
    return(NULL);
#endif
  } else if (memcmp(bytes,PNGsig,8)==0) {
#ifdef USE_GD_PNG
    img = gdImageCreateFromPng(stream);
    driver = "GD/PNG";
    image->img.gd = img;
    image->imagepath = NULL;
    image->imageurl = NULL;
    image->width = img->sx;
    image->height = img->sy;
#else
    msSetError(MS_MISCERR, "Unable to load PNG reference image.", "msImageLoadGD()");
    fclose(stream);
    return(NULL);
#endif
  } else if (memcmp(bytes,JPEGsig,3)==0) {
#ifdef USE_GD_JPEG
    img = gdImageCreateFromJpeg(stream);
    driver = "GD/JPEG";
    image->img.gd = img;
    image->imagepath = NULL;
    image->imageurl = NULL;
    image->width = img->sx;
    image->height = img->sy;
#else
    msSetError(MS_MISCERR, "Unable to load JPEG reference image.", "msImageLoadGD()");
    fclose(stream);
    return(NULL);
#endif
  }

  if(!img) {
    msSetError(MS_GDERR, "Unable to initialize image", "msLoadImageGDStream()");
    fclose(stream);
    return(NULL);
  }

  /* Create an outputFormatObj for the format. */
  image->format = msCreateDefaultOutputFormat( NULL, driver );
  image->format->refcount++;

  if( image->format == NULL )
  {
    msSetError(MS_GDERR, "Unable to create default OUTPUTFORMAT definition for driver '%s'.", "msImageLoadGDStream()", driver );
    return(NULL);
  }

  return image;
}

/* msImageLoadGDCtx creates an imageObj using the GD's IOCtx interface */
imageObj *msImageLoadGDCtx(gdIOCtx* ctx, const char *driver) {
    gdImagePtr img=NULL;
    imageObj *image = NULL;

    image = (imageObj *) calloc(1, sizeof(imageObj));

    if (strcasecmp(driver, "gd/gif") == MS_SUCCESS) {
#ifdef USE_GD_GIF
        img = gdImageCreateFromGifCtx(ctx);
#endif
    }
    if (strcasecmp(driver, "gd/png") == MS_SUCCESS) {
#ifdef USE_GD_PNG
        img = gdImageCreateFromPngCtx(ctx);
#endif
    }
    else if (strcasecmp(driver, "gd/jpeg") == MS_SUCCESS) {
#ifdef USE_GD_JPEG
        img = gdImageCreateFromJpegCtx(ctx);
#endif
    }

    if (!img) {
        msSetError(MS_GDERR, "Unable to initialize image",
                   "msLoadImageGDStream()");
        return(NULL);
    }

    image->img.gd = img;
    image->imagepath = NULL;
    image->imageurl = NULL;
    image->width = img->sx;
    image->height = img->sy;

    /* Create an outputFormatObj for the format. */
    image->format = msCreateDefaultOutputFormat( NULL, driver );
    image->format->refcount++;

    if( image->format == NULL ) {
        msSetError(MS_GDERR, "Unable to create default OUTPUTFORMAT definition for driver '%s'.", "msImageLoadGDStream()", driver );
        return(NULL);
    }

    return image;
}

/* msImageLoadGD now calls msImageLoadGDStream to do the work, change
 * made as part of the resolution of bug 550 */

imageObj *msImageLoadGD(const char *filename) {
    FILE *stream;
    imageObj *image;
    stream = fopen(filename, "rb");
    if (!stream) {
        msSetError(MS_IOERR, "(%s)", "msImageLoadGD()", filename );
        return(NULL);
    }
    image = msImageLoadGDStream(stream);
    if (!image) {
        msSetError(MS_GDERR, "Unable to initialize image '%s'", 
                   "msLoadImageGD()", filename);
        fclose(stream);
        return(NULL);
    }
    else return(image);
}

static gdImagePtr createBrush(gdImagePtr img, int width, int height, styleObj *style, int *fgcolor, int *bgcolor)
{
  gdImagePtr brush;

  if(!gdImageTrueColor(img)) {
    brush = gdImageCreate(width, height);
    if(style->backgroundcolor.pen >= 0)
      *bgcolor = gdImageColorAllocate(brush, style->backgroundcolor.red, style->backgroundcolor.green, style->backgroundcolor.blue);
    else {
      *bgcolor = gdImageColorAllocate(brush, gdImageRed(img,0), gdImageGreen(img, 0), gdImageBlue(img, 0));          
      gdImageColorTransparent(brush,0);
    }
    if(style->color.pen >= 0)
      *fgcolor = gdImageColorAllocate(brush, style->color.red, style->color.green, style->color.blue);
    else // try outline color
      *fgcolor = gdImageColorAllocate(brush, style->outlinecolor.red, style->outlinecolor.green, style->outlinecolor.blue); 
  } else {
    brush = gdImageCreateTrueColor(width, height);
    gdImageAlphaBlending(brush, 0);
    if(style->backgroundcolor.pen >= 0)
      *bgcolor = gdTrueColor(style->backgroundcolor.red, style->backgroundcolor.green, style->backgroundcolor.blue);      
    else
      *bgcolor = -1;      
    gdImageFilledRectangle(brush, 0, 0, width, height, *bgcolor);
    if(style->color.pen >= 0)
      *fgcolor = gdTrueColor(style->color.red, style->color.green, style->color.blue);
    else // try outline color
      *fgcolor = gdTrueColor(style->outlinecolor.red, style->outlinecolor.green, style->outlinecolor.blue);
  }

  return(brush);
}

// Function to create a custom hatch symbol.
static gdImagePtr createHatch(gdImagePtr img, int width, int height, rectObj *clip, styleObj *style)
{
  gdImagePtr hatch;
  int x1, x2, y1, y2;
  int size;
  double angle;
  int fg, bg;

  hatch = createBrush(img, width, height, style, &fg, &bg);

  if(style->antialias == MS_TRUE) {
    gdImageSetAntiAliased(hatch, fg);
    fg = gdAntiAliased;
  }

  if(style->size == -1)
      // TODO: Can we use msSymbolGetDefaultSize() here? See bug 751.
      size = 1;
  else
      size = style->size;

  // normalize the angle (180 to 0, 0 is east, 90 is north 180 is west)
  angle = fmod(style->angle, 360.0);
  if(angle < 0) angle += 360;
  if(angle >= 180) angle -= 180;

  if(angle >= 45 && angle <= 90) {
    x2 = (int)clip->minx; // 0
    y2 = (int)clip->miny; // 0
    y1 = (int)clip->maxy-1; // height-1 
    x1 = x2 - (y2 - y1)/tan(-MS_DEG_TO_RAD*angle);

    while(x1 < clip->maxx) { // width
      gdImageLine(hatch, x1, y1, x2, y2, fg);
      x1+=size;
      x2+=size; 
    }
  } else if(angle <= 135 && angle > 90) {
    x2 = (int)clip->minx; // 0
    y2 = (int)clip->maxy-1; // height-1
    y1 = (int)clip->miny; // 0
    x1 = x2 - (y2 - y1)/tan(-MS_DEG_TO_RAD*angle);

    while(x1 < clip->maxx) { // width
      gdImageLine(hatch, x1, y1, x2, y2, fg);
      x1+=size;
      x2+=size;
    }
  } else if(angle >= 0 && angle < 45) {    
    x1 = (int)clip->minx; // 0
    y1 = (int)clip->miny; // 0
    x2 = (int)clip->maxx-1; // width-1
    y2 = y1 + (x2 - x1)*tan(-MS_DEG_TO_RAD*angle);

    while(y2 < clip->maxy) { // height
      gdImageLine(hatch, x1, y1, x2, y2, fg);
      y1+=size;
      y2+=size;
    }
  } else if(angle < 180 && angle > 135) {
    x2 = (int)clip->maxx-1; // width-1
    y2 = (int)clip->miny; // 0
    x1 = (int)clip->minx; // 0
    y1 = y2 - (x2 - x1)*tan(-MS_DEG_TO_RAD*angle);

    while(y1 < clip->maxy) { // height
      gdImageLine(hatch, x1, y1, x2, y2, fg);
      y1+=size;
      y2+=size;
    }
  }

  return(hatch);
}

/*
** A series of low level drawing routines not available in GD, but specific to GD
*/
static void imageScanline(gdImagePtr im, int x1, int x2, int y, int c)
{
  int x;

  // TO DO: This fix (adding if/then) was to address circumstances in the polygon fill code
  // where x2 < x1. There may be something wrong in that code, but at any rate this is safer.

  if(x1 < x2)
    for(x=x1; x<=x2; x++) gdImageSetPixel(im, x, y, c);
  else
    for(x=x2; x<=x1; x++) gdImageSetPixel(im, x, y, c);
}


static double getAngleFromDxDy(double dx, double dy) 
{
  double angle;
  if (!dx) {
    if (dy > 0) 
      angle = MS_PI2;
    else
      angle = MS_3PI2;
  } else { 
    angle = atan(dy/dx);
    if (dx < 0) 
      angle += MS_PI;
    else if (dx > 0 && dy < 0) 
      angle += MS_2PI;
  }
  return(angle);
}

static gdPoint generateGDLineIntersection(gdPoint a, gdPoint b, gdPoint c, gdPoint d) 
{
  gdPoint p;
  double r;
  double denominator, numerator;

  if(b.x == c.x && b.y == c.y) return(b);

  numerator = ((a.y-c.y)*(d.x-c.x) - (a.x-c.x)*(d.y-c.y));  
  denominator = ((b.x-a.x)*(d.y-c.y) - (b.y-a.y)*(d.x-c.x));  

  r = numerator/denominator;

  p.x = MS_NINT(a.x + r*(b.x-a.x));
  p.y = MS_NINT(a.y + r*(b.y-a.y));

  return(p);
}

static void ImageFilledPolygonAA (gdImagePtr im, gdPointPtr p, int n, int c, int antialias)
{
  if (antialias) {
    //gdImageSetAntiAliased(im, c);
    gdImageSetAntiAliasedDontBlend(im, c, c);
    gdImageFilledPolygon(im, p, n, gdAntiAliased);
  } else {
    gdImageFilledPolygon(im, p, n, c);
  }
} 

static void imageFilledSegment(gdImagePtr im, double x, double y, double sz, double angle_from, double angle_to, int c, int antialias) 
{
  int j=0;
  double angle, dx, dy;
  static gdPoint points[38];
  double angle_dif;

  sz -= 0.1;
  if      (sz < 4)  angle_dif = MS_PI2;
  else if (sz < 12) angle_dif = MS_PI/5;
  else if (sz < 20) angle_dif = MS_PI2/7;
  else              angle_dif = MS_PI2/10;
  
  angle = angle_from;
  while(angle < angle_to) {
    dx = cos(angle)*sz;
    dy = sin(angle)*sz;
    points[j].x = MS_NINT(x + dx);
    points[j].y = MS_NINT(y + dy);
    angle += angle_dif;
    j++;
  }
  
  if (j) {
    dx = cos(angle_to)*sz;
    dy = sin(angle_to)*sz;
    points[j].x = MS_NINT(x + dx);
    points[j].y = MS_NINT(y + dy);
    j++;
    points[j].x = MS_NINT(x);
    points[j].y = MS_NINT(y);
    j++;
    points[j].x = points[0].x;
    points[j].y = points[0].y;
    j++;
  
//    gdImagePolygon(im, points, j, c);
//    gdImageFilledPolygon(im, points, j, c);
    ImageFilledPolygonAA(im, points, j, c, antialias);
  }
}

static void imageFilledCircle(gdImagePtr im, pointObj *p, int r, int c)
{
  int y;
  int ymin, ymax, xmin, xmax;
  double dx, dy;

  ymin = MS_MAX((p->y - r), 0);
  ymax = MS_MIN((p->y + r), (gdImageSY(im)-1));

  for(y=ymin; y<=ymax; y++) {
    dy = MS_ABS(p->y - y);
    dx = sqrt((r*r) - (dy*dy));

    xmin = MS_MAX((p->x - dx), 0);
    xmax = MS_MIN((p->x + dx), (gdImageSX(im)-1));

    imageScanline(im, xmin, xmax, y, c);
  }

  return;
}

static void imageOffsetPolyline(gdImagePtr img, shapeObj *p, int color, int offsetx, int offsety)
{
  int i, j, first;
  int dx, dy, dx0=0, dy0=0,  ox=0, oy=0, limit;
  double x, y, x0=0.0, y0=0.0, k=0.0, k0=0.0, q=0.0, q0=0.0;
  float par=0.71;

  if(offsety == -99) {
    limit = offsetx*offsetx/4;
    for (i = 0; i < p->numlines; i++) {
      first = 1;
      for(j=1; j<p->line[i].numpoints; j++) {        
        ox=0; oy=0;
        dx = p->line[i].point[j].x - p->line[i].point[j-1].x;
        dy = p->line[i].point[j].y - p->line[i].point[j-1].y;
        //offset setting - quick approximation, may be changed with goniometric functions
        if(dx==0) { //vertical line
          if(dy==0) continue; //checking unique points
          ox=(dy>0) ? -offsetx : offsetx;
        } else {
          k = (double)dy/(double)dx;
          if(MS_ABS(k)<0.5) {
            oy = (dx>0) ? offsetx : -offsetx;
          } else {
            if (MS_ABS(k)<2.1) {
              oy = (dx>0) ? offsetx*par : -offsetx*par;
              ox = (dy>0) ? -offsetx*par : offsetx*par;
            } else
              ox = (dy>0) ? -offsetx : offsetx;
          }
          q = p->line[i].point[j-1].y+oy - k*(p->line[i].point[j-1].x+ox);
        }
        //offset line points computation
        if(first==1) { // first point
          first = 0;
          x = p->line[i].point[j-1].x+ox;
          y = p->line[i].point[j-1].y+oy;
        } else { //middle points
          if((dx*dx+dy*dy)>limit){ //if the points are too close
            if(dx0==0) { //checking verticals
              if(dx==0) continue;
              x = x0;
              y = k*x + q;
            } else {
              if(dx==0) {
                x = p->line[i].point[j-1].x+ox;
                y = k0*x + q0;
              } else {
                if(k==k0) continue; //checking equal direction
                x = (q-q0)/(k0-k);
                y = k*x+q;
              }
            }
          }else{//need to be refined
            x = p->line[i].point[j-1].x+ox;
            y = p->line[i].point[j-1].y+oy;
          }
          gdImageLine(img, (int)x0, (int)y0, (int)x, (int)y, color);
        }
        dx0 = dx; dy0 = dy; x0 = x, y0 = y; k0 = k; q0=q;
      }
      //last point
      if(first==0)gdImageLine(img, (int)x0, (int)y0, p->line[i].point[p->line[i].numpoints-1].x+ox, p->line[i].point[p->line[i].numpoints-1].y+oy, color);
    }
  } else { // normal offset (eg. drop shadow)
    for (i = 0; i < p->numlines; i++)
      for(j=1; j<p->line[i].numpoints; j++)
        gdImageLine(img, (int)p->line[i].point[j-1].x+offsetx, (int)p->line[i].point[j-1].y+offsety, (int)p->line[i].point[j].x+offsetx, (int)p->line[i].point[j].y+offsety, color);
  }
}

typedef enum {CLIP_LEFT, CLIP_MIDDLE, CLIP_RIGHT} CLIP_STATE;

#define CLIP_CHECK(min, a, max) ((a) < (min) ? CLIP_LEFT : ((a) > (max) ? CLIP_RIGHT : CLIP_MIDDLE));
#define ROUND(a)       ( (a) + 0.5 )
#define SWAP( a, b, t) ( (t) = (a), (a) = (b), (b) = (t) )
#define EDGE_CHECK( x0, x, x1) ((x) < MS_MIN( (x0), (x1)) ? CLIP_LEFT : ((x) > MS_MAX( (x0), (x1)) ? CLIP_RIGHT : CLIP_MIDDLE ))

static void imagePolyline(gdImagePtr img, shapeObj *p, int color, int offsetx, int offsety)
{
  int i, j;
  
  if(offsetx != 0 || offsetx != 0) 
    imageOffsetPolyline(img, p, color, offsetx, offsety);
  else {
    for (i = 0; i < p->numlines; i++)
      for(j=1; j<p->line[i].numpoints; j++)
        gdImageLine(img, (int)p->line[i].point[j-1].x, (int)p->line[i].point[j-1].y, (int)p->line[i].point[j].x, (int)p->line[i].point[j].y, color);
  }
}

static void imageFilledPolygon(gdImagePtr im, shapeObj *p, int c, int offsetx, int offsety)
{
  float *slope;
  pointObj *point1, *point2, *testpoint1, *testpoint2;
  int i, j, k, l, m, nfound, *xintersect, temp, sign;
  int x, y, ymin, ymax, *horiz, wrong_order;
  int n;

  if(p->numlines == 0) return;
 
#if 0
  if( c & 0xFF000000 )
	gdImageAlphaBlending( im, 1 );
#endif
  
  /* calculate the total number of vertices */
  n=0;
  for(i=0; i<p->numlines; i++)
    n += p->line[i].numpoints;

  /* Allocate slope and horizontal detection arrays */
  slope = (float *)calloc(n, sizeof(float));
  horiz = (int *)calloc(n, sizeof(int));
  
  /* Since at most only one intersection is added per edge, there can only be at most n intersections per scanline */
  xintersect = (int *)calloc(n, sizeof(int));
  
  /* Find the min and max Y */
  ymin = p->line[0].point[0].y;
  ymax = ymin;
  
  for(l=0,j=0; j<p->numlines; j++) {
    point1 = &( p->line[j].point[p->line[j].numpoints-1] );
    for(i=0; i < p->line[j].numpoints; i++,l++) {
      point2 = &( p->line[j].point[i] );
      if(point1->y == point2->y) {
	horiz[l] = 1;
	slope[l] = 0.0;
      } else {
	horiz[l] = 0;
	slope[l] = (float) (point2->x - point1->x) / (point2->y - point1->y);
      }
      ymin = MS_MIN(ymin, point1->y);
      ymax = MS_MAX(ymax, point2->y);
      point1 = point2;
    }
  }  

  for(y = ymin; y <= ymax; y++) { /* for each scanline */

    nfound = 0;
    for(j=0, l=0; j<p->numlines; j++) { /* for each line, l is overall point counter */

      m = l; /* m is offset from begining of all vertices */
      point1 = &( p->line[j].point[p->line[j].numpoints-1] );
      for(i=0; i < p->line[j].numpoints; i++, l++) {
	point2 = &( p->line[j].point[i] );
	if(EDGE_CHECK(point1->y, y, point2->y) == CLIP_MIDDLE) {
	  
	  if(horiz[l]) /* First, is this edge horizontal ? */
	    continue;

	  /* Did we intersect the first point point ? */
	  if(y == point1->y) {
	    /* Yes, must find first non-horizontal edge */
	    k = i-1;
	    if(k < 0) k = p->line[j].numpoints-1;
	    while(horiz[m+k]) {
	      k--;
	      if(k < 0) k = p->line[j].numpoints-1;
	    }
	    /* Now perform sign test */
	    if(k > 0)
	      testpoint1 = &( p->line[j].point[k-1] );
	    else
	      testpoint1 = &( p->line[j].point[p->line[j].numpoints-1] );
	    testpoint2 = &( p->line[j].point[k] );
	    sign = (testpoint2->y - testpoint1->y) *
	      (point2->y - point1->y);
	    if(sign < 0)
	      xintersect[nfound++] = point1->x;
	    /* All done for point matching case */
	  } else {  
	    /* Not at the first point,
	       find the intersection*/
	    x = ROUND(point1->x + (y - point1->y)*slope[l]);
	    xintersect[nfound++] = x;
	  }
	}                 /* End of checking this edge */
	
	point1 = point2;  /* Go on to next edge */
      }
    } /* Finished this scanline, draw all of the spans */
    
    /* First, sort the intersections */
    do {
      wrong_order = 0;
      for(i=0; i < nfound-1; i++) {
	if(xintersect[i] > xintersect[i+1]) {
	  wrong_order = 1;
	  SWAP(xintersect[i], xintersect[i+1], temp);
	}
      }
    } while(wrong_order);
    
    /* Great, now we can draw the spans */
    for(i=0; i < nfound; i += 2)
      imageScanline(im, xintersect[i]+offsetx, xintersect[i+1]+offsetx, y+offsety, c);
  } /* End of scanline loop */
  
  /* Finally, draw all of the horizontal edges */
  for(j=0, l=0; j<p->numlines; j++) {
    point1 = &( p->line[j].point[p->line[j].numpoints - 1] );
    for(i=0; i<p->line[j].numpoints; i++, l++) {
      point2 = &( p->line[j].point[i] );
      if(horiz[l])
	imageScanline(im, point1->x+offsetx, point2->x+offsetx, point2->y+offsety, c);
      point1 = point2;
    }
  }
  
#if 0
   gdImageAlphaBlending( im, 0 );
#endif

  free(slope);
  free(horiz);
  free(xintersect);
}

/* ---------------------------------------------------------------------------*/
/*       Stroke an ellipse with a line symbol of the specified size and color */
/* ---------------------------------------------------------------------------*/
void msCircleDrawLineSymbolGD(symbolSetObj *symbolset, gdImagePtr img, pointObj *p, double r, styleObj *style, double scalefactor)
{
  int i, j;
  symbolObj *symbol;
  int styleDashed[100];
  int x, y, ox, oy;
  int bc, fc;
  int brush_bc, brush_fc;
  double size, d;
  gdImagePtr brush=NULL;
  gdPoint points[MS_MAXVECTORPOINTS];
  
  if(!p) return;

  if(style->backgroundcolor.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(style->backgroundcolor));
  if(style->color.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(style->color));
  if(style->outlinecolor.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(style->outlinecolor));
  
  symbol = &(symbolset->symbol[style->symbol]);
  bc = style->backgroundcolor.pen;
  fc = style->color.pen;
  if(fc==-1) fc = style->outlinecolor.pen;
  ox = style->offsetx; // TODO: add scaling?
  oy = style->offsety;

  if(style->size == -1) {
      size = msSymbolGetDefaultSize( &( symbolset->symbol[style->symbol] ) );
      size = MS_NINT(size*scalefactor);
  }
  else
  {
      size = MS_NINT(style->size*scalefactor);
  }
  size = MS_MAX(size, style->minsize);
  size = MS_MIN(size, style->maxsize);

  if(style->symbol > symbolset->numsymbols || style->symbol < 0) return; // no such symbol, 0 is OK
  if(fc < 0) return; // nothing to do
  if(size < 1) return; // size too small

  if(symbol == 0) { // just draw a single width line
    gdImageArc(img, (int)p->x + ox, (int)p->y + oy, (int)r, (int)r, 0, 360, fc);
    return;
  }

  switch(symbol->type) {
  case(MS_SYMBOL_SIMPLE):
    if(bc == -1) bc = gdTransparent;
    break;
  case(MS_SYMBOL_TRUETYPE):
    // msImageTruetypePolyline(img, p, symbol, fc, size, symbolset->fontset);
    return;
    break;
  case(MS_SYMBOL_CARTOLINE):
    return;
    break;
  case(MS_SYMBOL_ELLIPSE):   
    bc = gdTransparent;

    d = size/symbol->sizey;
    x = MS_NINT(symbol->sizex*d);    
    y = MS_NINT(symbol->sizey*d);
   
    if((x < 2) && (y < 2)) break;
    
    // create the brush image
    if((brush = searchImageCache(symbolset->imagecache, style, size)) == NULL) { 
      brush = createBrush(img, x, y, style, &brush_fc, &brush_bc); // not in cache, create

      x = MS_NINT(brush->sx/2); // center the ellipse
      y = MS_NINT(brush->sy/2);
      
      // draw in the brush image
      gdImageArc(brush, x, y, MS_NINT(d*symbol->points[0].x), MS_NINT(d*symbol->points[0].y), 0, 360, brush_fc);
      if(symbol->filled)
	gdImageFillToBorder(brush, x, y, brush_fc, brush_fc);
      
      symbolset->imagecache = addImageCache(symbolset->imagecache, &symbolset->imagecachesize, style, size, brush);
    }

    gdImageSetBrush(img, brush);
    fc=1; bc=0;
    break;
  case(MS_SYMBOL_PIXMAP):
    gdImageSetBrush(img, symbol->img);
    fc=1; bc=0;
    break;
  case(MS_SYMBOL_VECTOR):
    if(bc == -1) bc = gdTransparent;

    d = size/symbol->sizey;
    x = MS_NINT(symbol->sizex*d);    
    y = MS_NINT(symbol->sizey*d);

    if((x < 2) && (y < 2)) break;

    // create the brush image
    if((brush = searchImageCache(symbolset->imagecache, style, size)) == NULL) {
      brush = createBrush(img, x, y, style, &brush_fc, &brush_bc);  // not in cache, create

      // draw in the brush image 
      for(i=0;i < symbol->numpoints;i++) {
	points[i].x = MS_NINT(d*symbol->points[i].x);
	points[i].y = MS_NINT(d*symbol->points[i].y);
      }
      gdImageFilledPolygon(brush, points, symbol->numpoints, brush_fc);

      symbolset->imagecache = addImageCache(symbolset->imagecache, &symbolset->imagecachesize, style, size, brush);
    }

    gdImageSetBrush(img, brush);
    fc = 1; bc = 0;
    break;
  }  

  if(symbol->stylelength > 0) {
    int k=0, sc;
   
    sc = fc; // start with foreground color
    for(i=0; i<symbol->stylelength; i++) {      
      for(j=0; j<symbol->style[i]; j++) {
	styleDashed[k] = sc;
	k++;
      } 
      if(sc==fc) sc = bc; else sc = fc;
    }
    gdImageSetStyle(img, styleDashed, k);

    // gdImageArc(brush, x, y, MS_NINT(d*symbol->points[0].x), MS_NINT(d*symbol->points[0].y), 0, 360, brush_fc);

    if(!brush && !symbol->img)
      gdImageArc(img, (int)p->x + ox, (int)p->y + oy, (int)(2*r), (int)(2*r), 0, 360, gdStyled);      
    else 
      gdImageArc(img, (int)p->x + ox, (int)p->y + oy, (int)(2*r), (int)(2*r), 0, 360, gdStyledBrushed);
  } else {
    if(!brush && !symbol->img)
      gdImageArc(img, (int)p->x + ox, (int)p->y + oy, (int)(2*r), (int)(2*r), 0, 360, fc);
    else
      gdImageArc(img, (int)p->x + ox, (int)p->y + oy, (int)(2*r), (int)(2*r), 0, 360, gdBrushed);
  }

  return;
}

/* ------------------------------------------------------------------------------- */
/*       Fill a circle with a shade symbol of the specified size and color       */
/* ------------------------------------------------------------------------------- */
void msCircleDrawShadeSymbolGD(symbolSetObj *symbolset, gdImagePtr img, 
                               pointObj *p, double r, styleObj *style, double scalefactor)
{
  symbolObj *symbol;
  int i;
  gdPoint oldpnt,newpnt;
  gdPoint sPoints[MS_MAXVECTORPOINTS];
  gdImagePtr tile=NULL;
  int x, y, ox, oy;

  int fc, bc, oc;
  int tile_bc=-1, tile_fc=-1; /* colors (background and foreground) */
  double size, d;  

  int bbox[8];
  rectObj rect;
  char *font=NULL;

  if(!p) return;

  if(!MS_VALID_COLOR(style->color) && MS_VALID_COLOR(style->outlinecolor)) { // use msDrawLineSymbolGD() instead (POLYLINE)
    msCircleDrawLineSymbolGD(symbolset, img, p, r, style, scalefactor);
    return;
  }

  if(style->backgroundcolor.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(style->backgroundcolor));
  if(style->color.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(style->color));
  if(style->outlinecolor.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(style->outlinecolor));

  symbol = &(symbolset->symbol[style->symbol]);
  bc = style->backgroundcolor.pen;
  fc = style->color.pen;
  oc = style->outlinecolor.pen;
  ox = style->offsetx; // TODO: add scaling?
  oy = style->offsety;

  if(style->size == -1) {
      size = msSymbolGetDefaultSize( &( symbolset->symbol[style->symbol] ) );
      size = MS_NINT(size*scalefactor);
  }
  else
  {
      size = MS_NINT(style->size*scalefactor);
  }
  size = MS_MAX(size, style->minsize);
  size = MS_MIN(size, style->maxsize);

  if(style->symbol > symbolset->numsymbols || style->symbol < 0) return; // no such symbol, 0 is OK
  if(fc < 0) return; // invalid color, -1 is valid
  if(size < 1) return; // size too small
      
  if(style->symbol == 0) { // simply draw a single pixel of the specified color    
    imageFilledCircle(img, p, r, fc);
    if(oc>-1) gdImageArc(img, (int)p->x, (int)p->y, (int)(2*r), (int)(2*r), 0, 360, oc);
    return;
  }

  switch(symbol->type) {
  case(MS_SYMBOL_TRUETYPE):    
    
#ifdef USE_GD_FT
    font = msLookupHashTable(&(symbolset->fontset->fonts), symbol->font);
    if(!font) return;

    if(msGetCharacterSize(symbol->character, size, font, &rect) != MS_SUCCESS) return;
    x = rect.maxx - rect.minx;
    y = rect.maxy - rect.miny;

    tile = createBrush(img, x, y, style, &tile_fc, &tile_bc); // create the tile image

    x = -rect.minx; // center the glyph
    y = -rect.miny;

    gdImageStringFT(tile, bbox, ((symbol->antialias)?(tile_fc):-(tile_fc)), font, size, 0, x, y, symbol->character);
    gdImageSetTile(img, tile);
#endif

    break;
  case(MS_SYMBOL_PIXMAP):

    // fill with the background color before drawing transparent symbol
    if(symbol->transparent == MS_TRUE && bc > -1)
      imageFilledCircle(img, p, r, bc);
    
    gdImageSetTile(img, symbol->img);

    break;
  case(MS_SYMBOL_ELLIPSE):    
   
    d = size/symbol->sizey; // size ~ height in pixels
    x = MS_NINT(symbol->sizex*d)+1;
    y = MS_NINT(symbol->sizey*d)+1;

    if((x <= 1) && (y <= 1)) { // No sense using a tile, just fill solid
      imageFilledCircle(img, p, r, fc);
      if(oc>-1) gdImageArc(img, (int)p->x, (int)p->y, (int)(2*r), (int)(2*r), 0, 360, oc);
      return;
    }

    tile = createBrush(img, x, y, style, &tile_fc, &tile_bc); // create tile image
    
    x = MS_NINT(tile->sx/2); // center the ellipse
    y = MS_NINT(tile->sy/2);
    
    // draw in the tile image
    gdImageArc(tile, x, y, MS_NINT(d*symbol->points[0].x), MS_NINT(d*symbol->points[0].y), 0, 360, tile_fc);
    if(symbol->filled)
      gdImageFillToBorder(tile, x, y, tile_fc, tile_fc);

    gdImageSetTile(img, tile);
 
    break;
  case(MS_SYMBOL_VECTOR):

    if(fc < 0)
      return;

    d = size/symbol->sizey; // size ~ height in pixels
    x = MS_NINT(symbol->sizex*d)+1;    
    y = MS_NINT(symbol->sizey*d)+1;

    if((x <= 1) && (y <= 1)) { // No sense using a tile, just fill solid
      imageFilledCircle(img, p, r, fc);
      if(oc>-1) gdImageArc(img, (int)p->x, (int)p->y, (int)(2*r), (int)(2*r), 0, 360, oc);
      return;
    }

    tile = createBrush(img, x, y, style, &tile_fc, &tile_bc); // create tile image

    // draw in the tile image
    if(symbol->filled) {

      for(i=0;i < symbol->numpoints;i++) {
	sPoints[i].x = MS_NINT(d*symbol->points[i].x);
	sPoints[i].y = MS_NINT(d*symbol->points[i].y);
      }
      gdImageFilledPolygon(tile, sPoints, symbol->numpoints, tile_fc);      

    } else  { /* shade is a vector drawing */
      
      oldpnt.x = MS_NINT(d*symbol->points[0].x); /* convert first point in shade smbol */
      oldpnt.y = MS_NINT(d*symbol->points[0].y);

      /* step through the shade symbol */
      for(i=1;i < symbol->numpoints;i++) {
	if((symbol->points[i].x < 0) && (symbol->points[i].y < 0)) {
	  oldpnt.x = MS_NINT(d*symbol->points[i].x);
	  oldpnt.y = MS_NINT(d*symbol->points[i].y);
	} else {
	  if((symbol->points[i-1].x < 0) && (symbol->points[i-1].y < 0)) { /* Last point was PENUP, now a new beginning */
	    oldpnt.x = MS_NINT(d*symbol->points[i].x);
	    oldpnt.y = MS_NINT(d*symbol->points[i].y);
	  } else {
	    newpnt.x = MS_NINT(d*symbol->points[i].x);
	    newpnt.y = MS_NINT(d*symbol->points[i].y);
	    gdImageLine(tile, oldpnt.x, oldpnt.y, newpnt.x, newpnt.y, tile_fc);
	    oldpnt = newpnt;
	  }
	}
      } /* end for loop */
    }

    gdImageSetTile(img, tile);
 
    break;
  default:
    break;
  }

  // fill the circle in the main image
  imageFilledCircle(img, p, r, gdTiled);
  if(oc>-1) gdImageArc(img, (int)p->x, (int)p->y, (int)(2*r), (int)(2*r), 0, 360, oc);
  if(tile) gdImageDestroy(tile);

  return;
}


/* ------------------------------------------------------------------------------- */
/*       Draw a single marker symbol of the specified size and color               */
/* ------------------------------------------------------------------------------- */
void msDrawMarkerSymbolGD(symbolSetObj *symbolset, gdImagePtr img, pointObj *p, styleObj *style, double scalefactor)
{
  symbolObj *symbol;
  int offset_x, offset_y, x, y;
  int j;
  gdPoint oldpnt,newpnt;
  gdPoint mPoints[MS_MAXVECTORPOINTS];
  char *error=NULL;

  gdImagePtr tmp;
  int tmp_fc=-1, tmp_bc, tmp_oc=-1;
  int fc, bc, oc;
  double size,d;

  int ox, oy;
  
  int bbox[8];
  rectObj rect;
  char *font=NULL;

  if(!p) return;

  if(style->backgroundcolor.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(style->backgroundcolor));
  if(style->color.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(style->color));
  if(style->outlinecolor.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(style->outlinecolor));

  symbol = &(symbolset->symbol[style->symbol]);
  bc = style->backgroundcolor.pen;
  fc = style->color.pen;
  oc = style->outlinecolor.pen;
  ox = style->offsetx; // TODO: add scaling?
  oy = style->offsety;

  if(style->size == -1) {
      size = msSymbolGetDefaultSize( &( symbolset->symbol[style->symbol] ) );
      size = MS_NINT(size*scalefactor);
  }
  else
  {
      size = MS_NINT(style->size*scalefactor);
  }
  size = MS_MAX(size, style->minsize);
  size = MS_MIN(size, style->maxsize);

  if(style->symbol > symbolset->numsymbols || style->symbol < 0) return; // no such symbol, 0 is OK
  if(fc<0 && oc<0 && symbol->type != MS_SYMBOL_PIXMAP) return; // nothing to do (color not required for a pixmap symbol)
  if(size < 1) return; // size too small

  if(style->symbol == 0 && fc >= 0) { /* simply draw a single pixel of the specified color */
    gdImageSetPixel(img, p->x + ox, p->y + oy, fc);
    return;
  }  

  switch(symbol->type) {  
  case(MS_SYMBOL_TRUETYPE): // TODO: Need to leverage the image cache!

#ifdef USE_GD_FT
    font = msLookupHashTable(&(symbolset->fontset->fonts), symbol->font);
    if(!font) return;

    if(msGetCharacterSize(symbol->character, size, font, &rect) != MS_SUCCESS) return;

    x = p->x + ox - (rect.maxx - rect.minx)/2 - rect.minx;
    y = p->y + oy - rect.maxy + (rect.maxy - rect.miny)/2;  

    // ============== MOD BY DHC MAR 14, 2003 -- Can we get outline color (and rotation) for truetype symbols?
    if( oc >= 0 ) {
      error = gdImageStringFT(img, bbox, ((symbol->antialias)?(oc):-(oc)), font, size, 0, x, y-1, symbol->character);
      if(error) {
	msSetError(MS_TTFERR, error, "msDrawMarkerSymbolGD()");
	return;
      }

      gdImageStringFT(img, bbox, ((symbol->antialias)?(oc):-(oc)), font, size, 0, x, y+1, symbol->character);
      gdImageStringFT(img, bbox, ((symbol->antialias)?(oc):-(oc)), font, size, 0, x+1, y, symbol->character);
      gdImageStringFT(img, bbox, ((symbol->antialias)?(oc):-(oc)), font, size, 0, x-1, y, symbol->character);
      gdImageStringFT(img, bbox, ((symbol->antialias)?(oc):-(oc)), font, size, 0, x+1, y+1, symbol->character);
      gdImageStringFT(img, bbox, ((symbol->antialias)?(oc):-(oc)), font, size, 0, x+1, y-1, symbol->character);
      gdImageStringFT(img, bbox, ((symbol->antialias)?(oc):-(oc)), font, size, 0, x-1, y+1, symbol->character);
      gdImageStringFT(img, bbox, ((symbol->antialias)?(oc):-(oc)), font, size, 0, x-1, y-1, symbol->character);
    }
    // END OF DHC MOD

    gdImageStringFT(img, bbox, ((symbol->antialias)?(fc):-(fc)), font, size, 0, x, y, symbol->character);
#endif

    break;
  case(MS_SYMBOL_PIXMAP):
    if(size == 1) { // don't scale
      offset_x = MS_NINT(p->x - .5*symbol->img->sx + ox);
      offset_y = MS_NINT(p->y - .5*symbol->img->sy + oy);
      gdImageCopy(img, symbol->img, offset_x, offset_y, 0, 0, symbol->img->sx, symbol->img->sy);
    } else {
      d = size/symbol->img->sy;
      offset_x = MS_NINT(p->x - .5*symbol->img->sx*d + ox);
      offset_y = MS_NINT(p->y - .5*symbol->img->sy*d + oy);
      // gdImageCopyResized(img, symbol->img, offset_x, offset_y, 0, 0, symbol->img->sx*d, symbol->img->sy*d, symbol->img->sx, symbol->img->sy);
      gdImageCopyResampled(img, symbol->img, offset_x, offset_y, 0, 0, symbol->img->sx*d, symbol->img->sy*d, symbol->img->sx, symbol->img->sy);
    }
    break;
  case(MS_SYMBOL_ELLIPSE): // TODO: Need to leverage the image cache!
    d = size/symbol->sizey;
    x = MS_NINT(symbol->sizex*d)+1;
    y = MS_NINT(symbol->sizey*d)+1;

    /* create temporary image and allocate a few colors */
    tmp = gdImageCreate(x, y);
    tmp_bc = gdImageColorAllocate(tmp, gdImageRed(img, 0), gdImageGreen(img, 0), gdImageBlue(img, 0));
    gdImageColorTransparent(tmp, 0);
    if(fc >= 0)
      tmp_fc = gdImageColorAllocate(tmp, gdImageRed(img, fc), gdImageGreen(img, fc), gdImageBlue(img, fc));
    if(oc >= 0)
      tmp_oc = gdImageColorAllocate(tmp, gdImageRed(img, oc), gdImageGreen(img, oc), gdImageBlue(img, oc));

    x = MS_NINT(tmp->sx/2);
    y = MS_NINT(tmp->sy/2);

    /* draw in the temporary image */
    if(tmp_oc >= 0) {
      gdImageArc(tmp, x, y, MS_NINT(d*symbol->points[0].x), MS_NINT(d*symbol->points[0].y), 0, 360, tmp_oc);
      if(symbol->filled && tmp_fc >= 0)
	gdImageFillToBorder(tmp, x, y, tmp_oc, tmp_fc);
    } else {
      if(tmp_fc >= 0) {
	gdImageArc(tmp, x, y, MS_NINT(d*symbol->points[0].x), MS_NINT(d*symbol->points[0].y), 0, 360, tmp_fc);
	if(symbol->filled)
	  gdImageFillToBorder(tmp, x, y, tmp_fc, tmp_fc);
      }
    }

    /* paste the tmp image in the main image */
    offset_x = MS_NINT(p->x - .5*tmp->sx + ox);
    offset_y = MS_NINT(p->y - .5*tmp->sx + oy);
    msFixedImageCopy(img, tmp, offset_x, offset_y, 0, 0, tmp->sx, tmp->sy);

    gdImageDestroy(tmp);

    break;
  case(MS_SYMBOL_VECTOR): // TODO: Need to leverage the image cache!
    d = size/symbol->sizey;
    offset_x = MS_NINT(p->x - d*.5*symbol->sizex + ox);
    offset_y = MS_NINT(p->y - d*.5*symbol->sizey + oy);
    
    if(symbol->filled) { /* if filled */
      for(j=0;j < symbol->numpoints;j++) {
	mPoints[j].x = MS_NINT(d*symbol->points[j].x + offset_x);
	mPoints[j].y = MS_NINT(d*symbol->points[j].y + offset_y);
      }
      if(fc >= 0)
	gdImageFilledPolygon(img, mPoints, symbol->numpoints, fc);
      if(oc >= 0)
	gdImagePolygon(img, mPoints, symbol->numpoints, oc);
      
    } else  { /* NOT filled */     

      if(fc < 0) return;
      
      oldpnt.x = MS_NINT(d*symbol->points[0].x + offset_x); /* convert first point in marker s */
      oldpnt.y = MS_NINT(d*symbol->points[0].y + offset_y);
      
      for(j=1;j < symbol->numpoints;j++) { /* step through the marker s */
	if((symbol->points[j].x < 0) && (symbol->points[j].x < 0)) {
	  oldpnt.x = MS_NINT(d*symbol->points[j].x + offset_x);
	  oldpnt.y = MS_NINT(d*symbol->points[j].y + offset_y);
	} else {
	  if((symbol->points[j-1].x < 0) && (symbol->points[j-1].y < 0)) { /* Last point was PENUP, now a new beginning */
	    oldpnt.x = MS_NINT(d*symbol->points[j].x + offset_x);
	    oldpnt.y = MS_NINT(d*symbol->points[j].y + offset_y);
	  } else {
	    newpnt.x = MS_NINT(d*symbol->points[j].x + offset_x);
	    newpnt.y = MS_NINT(d*symbol->points[j].y + offset_y);
	    gdImageLine(img, oldpnt.x, oldpnt.y, newpnt.x, newpnt.y, fc);
	    oldpnt = newpnt;
	  }
	}
      } /* end for loop */      
    } /* end if-then-else */
    break;
  default:
    break;
  } /* end switch statement */

  return;
}

/* ------------------------------------------------------------------------------- */
/*       Draw a line symbol of the specified size and color                        */
/* ------------------------------------------------------------------------------- */
void msDrawLineSymbolGD(symbolSetObj *symbolset, gdImagePtr img, shapeObj *p, styleObj *style, double scalefactor)
{
  int i, j;
  symbolObj *symbol;
  int styleDashed[100];
  int x, y;
  int ox, oy;
  int fc, bc;
  int brush_bc, brush_fc;
  double size, d;
  gdImagePtr brush=NULL;
  gdPoint points[MS_MAXVECTORPOINTS];

  if(!p) return;
  if(p->numlines <= 0) return;

  if(style->backgroundcolor.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(style->backgroundcolor));
  if(style->outlinecolor.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(style->outlinecolor));
  if(style->color.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(style->color));

  symbol = &(symbolset->symbol[style->symbol]);
  bc = style->backgroundcolor.pen;
  fc = style->color.pen;
  if(fc==-1) fc = style->outlinecolor.pen;

  if(style->size == -1) {
      size = msSymbolGetDefaultSize( &( symbolset->symbol[style->symbol] ) );
  }
  else
      size = style->size;

  // TODO: Don't get this modification, is it needed elsewhere?
  if(size*scalefactor > style->maxsize) scalefactor = (float)style->maxsize/(float)size;
  if(size*scalefactor < style->minsize) scalefactor = (float)style->minsize/(float)size;
  size = MS_NINT(size*scalefactor);

  //size = MS_MAX(size, style->minsize);
  //size = MS_MIN(size, style->maxsize);

  if(style->symbol > symbolset->numsymbols || style->symbol < 0) return; // no such symbol, 0 is OK
  if(fc < 0 && symbol->type != MS_SYMBOL_PIXMAP) return; // nothing to do (color not required for a pixmap symbol)
  if(size < 1) return; // size too small
  ox = MS_NINT(style->offsetx*scalefactor);
  oy = (style->offsety == -99) ? -99 : style->offsety*scalefactor;

  if(style->symbol == 0) { // just draw a single width line
    imagePolyline(img, p, fc, ox, oy);
    return;
  }

  switch(symbol->type) {
  case(MS_SYMBOL_SIMPLE):
    if(bc == -1) bc = gdTransparent;
    break;
  case(MS_SYMBOL_TRUETYPE):
    msImageTruetypePolyline(symbolset, img, p, style, scalefactor);
    return;
    break;
  case(MS_SYMBOL_CARTOLINE):
    msImageCartographicPolyline(img, p, style, symbol, fc, size, scalefactor);
    return;
    break;
  case(MS_SYMBOL_ELLIPSE):
    bc = gdTransparent;

    d = (size)/symbol->sizey;
    x = MS_NINT(symbol->sizex*d);    
    y = MS_NINT(symbol->sizey*d);
    
    if((x < 2) && (y < 2)) break;
    
    // create the brush image
    if((brush = searchImageCache(symbolset->imagecache, style, size)) == NULL) { 
      brush = createBrush(img, x, y, style, &brush_fc, &brush_bc); // not in cache, create it

      x = MS_NINT(brush->sx/2); // center the ellipse
      y = MS_NINT(brush->sy/2);
      
      // draw in the brush image
      gdImageArc(brush, x, y, MS_NINT(d*symbol->points[0].x), MS_NINT(d*symbol->points[0].y), 0, 360, brush_fc);
      if(symbol->filled)
        gdImageFillToBorder(brush, x, y, brush_fc, brush_fc);
      
      symbolset->imagecache = addImageCache(symbolset->imagecache, &symbolset->imagecachesize, style, size, brush);
    }

    gdImageSetBrush(img, brush);
    fc = 1; bc = 0;
    break;
  case(MS_SYMBOL_PIXMAP):
    gdImageSetBrush(img, symbol->img);
    fc = 1; bc = 0;
    break;
  case(MS_SYMBOL_VECTOR):
    if(bc == -1) bc = gdTransparent;

    d = size/symbol->sizey;
    x = MS_NINT(symbol->sizex*d);    
    y = MS_NINT(symbol->sizey*d);

    if((x < 2) && (y < 2)) break;

    // create the brush image
    if((brush = searchImageCache(symbolset->imagecache, style, size)) == NULL) { 
      brush = createBrush(img, x, y, style, &brush_fc, &brush_bc); // not in cache, create it

      // draw in the brush image 
      for(i=0;i < symbol->numpoints;i++) {
        points[i].x = MS_NINT(d*symbol->points[i].x);
        points[i].y = MS_NINT(d*symbol->points[i].y);
      }
      gdImageFilledPolygon(brush, points, symbol->numpoints, brush_fc);

      symbolset->imagecache = addImageCache(symbolset->imagecache, &symbolset->imagecachesize, style, size, brush);
    }

    gdImageSetBrush(img, brush);
    fc = 1; bc = 0;
    break;
  }  

  if(symbol->stylelength > 0) {
    int k=0, sc;
   
    sc = fc; // start with foreground color
    for(i=0; i<symbol->stylelength; i++) {      
      for(j=0; j<symbol->style[i]; j++) {
        styleDashed[k] = sc;
        k++;
      } 
      if(sc==fc) sc = bc; else sc = fc;
    }
    gdImageSetStyle(img, styleDashed, k);

    if(!brush && !symbol->img)
      imagePolyline(img, p, gdStyled, ox, oy);
    else 
      imagePolyline(img, p, gdStyledBrushed, ox, oy);
  } else {
    if(!brush && !symbol->img) {
      if(style->antialias==MS_TRUE) {       
        gdImageSetAntiAliased(img, fc);
        imagePolyline(img, p, gdAntiAliased, ox, oy);
      } else
        imagePolyline(img, p, fc, ox, oy);
    } else
      imagePolyline(img, p, gdBrushed, ox, oy);
  }

  return;
}

/* ------------------------------------------------------------------------------- */
/*       Draw a shade symbol of the specified size and color                       */
/* ------------------------------------------------------------------------------- */
void msDrawShadeSymbolGD(symbolSetObj *symbolset, gdImagePtr img, shapeObj *p, styleObj *style, double scalefactor)
{
  symbolObj *symbol;
  int i;
  gdPoint oldpnt, newpnt;
  gdPoint sPoints[MS_MAXVECTORPOINTS];
  gdImagePtr tile;
  int x, y, ox, oy;
  int tile_bc=-1, tile_fc=-1; /* colors (background and foreground) */
  int fc, bc, oc;
  double size, d;
  
  int bbox[8];
  rectObj rect;
  char *font=NULL;

  if(!p) return;
  if(p->numlines <= 0) return;

  if(style->backgroundcolor.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(style->backgroundcolor));
  if(style->color.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(style->color));
  if(style->outlinecolor.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(style->outlinecolor));

  symbol = &(symbolset->symbol[style->symbol]);
  bc = style->backgroundcolor.pen;
  fc = style->color.pen;
  oc = style->outlinecolor.pen;
  if(style->size == -1) {
      size = msSymbolGetDefaultSize( &( symbolset->symbol[style->symbol] ) );
      size = MS_NINT(size*scalefactor);
  }
  else
      size = MS_NINT(style->size*scalefactor);
  size = MS_MAX(size, style->minsize);
  size = MS_MIN(size, style->maxsize);
  ox = MS_NINT(style->offsetx*scalefactor); // should we scale the offsets?
  oy = MS_NINT(style->offsety*scalefactor);

  if(fc==-1 && oc!=-1) { // use msDrawLineSymbolGD() instead (POLYLINE)
    msDrawLineSymbolGD(symbolset, img, p, style, scalefactor);
    return;
  }

  if(style->symbol > symbolset->numsymbols || style->symbol < 0) return; // no such symbol, 0 is OK  
  if(fc < 0) return; // nothing to do
  if(size < 1) return; // size too small
      
  if(style->symbol == 0) { /* simply draw a single pixel of the specified color */    
    if(style->antialias==MS_TRUE) {      
      imageFilledPolygon(img, p, fc, ox, oy); // fill is NOT anti-aliased, the outline IS
      if(oc>-1)
        gdImageSetAntiAliased(img, oc);
      else
	gdImageSetAntiAliased(img, fc);
      imagePolyline(img, p, gdAntiAliased, ox, oy);
    } else {
      imageFilledPolygon(img, p, fc, ox, oy);
      if(oc>-1) imagePolyline(img, p, oc, ox, oy);
    }
    return;
  }
  
  switch(symbol->type) {
  case(MS_SYMBOL_HATCH):

    msComputeBounds(p); // we need to know how big to make the tile
    // tile = createHatch(img, (p->bounds.maxx-p->bounds.minx), (p->bounds.maxy-p->bounds.miny), style);    
    tile = createHatch(img, img->sx, img->sy, &p->bounds, style);

    gdImageSetTile(img, tile);
    imageFilledPolygon(img, p, gdTiled, ox, oy);

    if(style->antialias==MS_TRUE && oc>-1) {
      gdImageSetAntiAliased(img, oc);
      imagePolyline(img, p, gdAntiAliased, ox, oy);
    } else
      if(oc>-1) imagePolyline(img, p, oc, ox, oy);

    gdImageDestroy(tile);

    break;
  case(MS_SYMBOL_TRUETYPE):    
    
#ifdef USE_GD_FT
    font = msLookupHashTable(&(symbolset->fontset->fonts), symbol->font);
    if(!font) return;

    if(msGetCharacterSize(symbol->character, size, font, &rect) != MS_SUCCESS) return;
    x = rect.maxx - rect.minx;
    y = rect.maxy - rect.miny;
    
    // create tile image
    tile = createBrush(img, x, y, style, &tile_fc, &tile_bc);

    // center the glyph
    x = -rect.minx;
    y = -rect.miny;

    // TODO, should style->antialias be used here (it'd be nice to ditch antialias from symbols)
    gdImageStringFT(tile, bbox, ((symbol->antialias)?(tile_fc):-(tile_fc)), font, size, 0, x, y, symbol->character);

    gdImageSetTile(img, tile);
    imageFilledPolygon(img, p, gdTiled, ox, oy);

    if(style->antialias==MS_TRUE && oc>-1) { 
      gdImageSetAntiAliased(img, oc);     
      imagePolyline(img, p, gdAntiAliased, ox, oy);
    } else 
      if(oc>-1) imagePolyline(img, p, oc, ox, oy);

    gdImageDestroy(tile);
#endif

    break;
  case(MS_SYMBOL_PIXMAP):
    // fill with the background color before drawing transparent symbol
    if(symbol->transparent == MS_TRUE && bc > -1)
      imageFilledPolygon(img, p, bc, ox, oy); 
    
    gdImageSetTile(img, symbol->img);
    imageFilledPolygon(img, p, gdTiled, ox, oy);

    if(style->antialias==MS_TRUE && oc>-1) { 
      gdImageSetAntiAliased(img, oc);     
      imagePolyline(img, p, gdAntiAliased, ox, oy);
    } else 
      if(oc>-1) imagePolyline(img, p, oc, ox, oy);

    break;
  case(MS_SYMBOL_ELLIPSE):    
    d = size/symbol->sizey; // size ~ height in pixels
    x = MS_NINT(symbol->sizex*d)+1;
    y = MS_NINT(symbol->sizey*d)+1;

    if((x <= 1) && (y <= 1)) { /* No sense using a tile, just fill solid */
      if(style->antialias==MS_TRUE) {      
        imageFilledPolygon(img, p, fc, ox, oy); // fill is NOT anti-aliased, the outline IS
        if(oc>-1)
          gdImageSetAntiAliased(img, oc);
        else
	  gdImageSetAntiAliased(img, fc);
        imagePolyline(img, p, gdAntiAliased, ox, oy);
      } else {
        imageFilledPolygon(img, p, fc, oy, oy);
        if(oc>-1) imagePolyline(img, p, oc, ox, oy);
      }
      return;
    }
    
    // create tile image
    tile = createBrush(img, x, y, style, &tile_fc, &tile_bc);

    // center ellipse
    x = MS_NINT(tile->sx/2);
    y = MS_NINT(tile->sy/2);

    // draw in tile image
    gdImageArc(tile, x, y, MS_NINT(d*symbol->points[0].x), MS_NINT(d*symbol->points[0].y), 0, 360, tile_fc);
    if(symbol->filled)
      gdImageFillToBorder(tile, x, y, tile_fc, tile_fc);

    // fill the polygon in the main image
    gdImageSetTile(img, tile);
    imageFilledPolygon(img, p, gdTiled, ox, oy);

    if(style->antialias==MS_TRUE && oc>-1) { 
      gdImageSetAntiAliased(img, oc);     
      imagePolyline(img, p, gdAntiAliased, ox, oy);
    } else 
      if(oc>-1) imagePolyline(img, p, oc, ox, oy);

    gdImageDestroy(tile);
    break;
  case(MS_SYMBOL_VECTOR):
    d = size/symbol->sizey; // size ~ height in pixels
    x = MS_NINT(symbol->sizex*d)+1;    
    y = MS_NINT(symbol->sizey*d)+1;

    if((x <= 1) && (y <= 1)) { /* No sense using a tile, just fill solid */
      if(style->antialias==MS_TRUE) {      
        imageFilledPolygon(img, p, fc, ox, oy); // fill is NOT anti-aliased, the outline IS
        if(oc>-1)
          gdImageSetAntiAliased(img, oc);
        else
	  gdImageSetAntiAliased(img, fc);
        imagePolyline(img, p, gdAntiAliased, ox, oy);
      } else {
        imageFilledPolygon(img, p, fc, ox, oy);
        if(oc>-1) imagePolyline(img, p, oc, ox, oy);
      }      
      return;
    }

    // create tile image
    tile = createBrush(img, x, y, style, &tile_fc, &tile_bc);
   
    // draw in the tile image    
    if(symbol->filled) {

      for(i=0;i < symbol->numpoints;i++) {
	sPoints[i].x = MS_NINT(d*symbol->points[i].x);
	sPoints[i].y = MS_NINT(d*symbol->points[i].y);
      }
      gdImageFilledPolygon(tile, sPoints, symbol->numpoints, tile_fc);      

    } else  { /* shade is a vector drawing */
      
      oldpnt.x = MS_NINT(d*symbol->points[0].x); /* convert first point in shade smbol */
      oldpnt.y = MS_NINT(d*symbol->points[0].y);

      /* step through the shade sy */
      for(i=1;i < symbol->numpoints;i++) {
	if((symbol->points[i].x < 0) && (symbol->points[i].y < 0)) {
	  oldpnt.x = MS_NINT(d*symbol->points[i].x);
	  oldpnt.y = MS_NINT(d*symbol->points[i].y);
	} else {
	  if((symbol->points[i-1].x < 0) && (symbol->points[i-1].y < 0)) { /* Last point was PENUP, now a new beginning */
	    oldpnt.x = MS_NINT(d*symbol->points[i].x);
	    oldpnt.y = MS_NINT(d*symbol->points[i].y);
	  } else {
	    newpnt.x = MS_NINT(d*symbol->points[i].x);
	    newpnt.y = MS_NINT(d*symbol->points[i].y);
	    gdImageLine(tile, oldpnt.x, oldpnt.y, newpnt.x, newpnt.y, tile_fc);
	    oldpnt = newpnt;
	  }
	}
      } /* end for loop */
    }

    // Fill the polygon in the main image
    gdImageSetTile(img, tile);
    imageFilledPolygon(img, p, gdTiled, ox, oy);

    if(style->antialias==MS_TRUE && oc>-1) { 
      gdImageSetAntiAliased(img, oc);     
      imagePolyline(img, p, gdAntiAliased, ox, oy);
    } else
      if(oc>-1) imagePolyline(img, p, oc, ox, oy);

    gdImageDestroy(tile);
    break;
  default:
    break;
  }

  return;
}

/* ---------------------------------------------------------- */
/*  The basic function for drawing a cartographic line symbol */
/*  of the specified size, color                              */
/* ---------------------------------------------------------- */
static void RenderCartoLine(gdImagePtr img, int gox, double *acoord, double *bcoord, double da, double db, double angle_n, double size, int c, symbolObj *symbol, double styleCoef, double *styleSize, int *styleIndex, int *styleVis, gdPoint *points, double *da_px, double *db_px, int offseta, int offsetb, int antialias) 
{
  double an, bn, da_pxn, db_pxn, d_size, a, b;
  double d_step_coef, da_pxn_coef, db_pxn_coef, da_px_coef, db_px_coef;
  int db_line, first, drawpoly=0;
  gdPoint poly_points[4];
  static double last_style_size;

  /* Steps for one pixel */
  *da_px = da/MS_ABS(db);
  *db_px = MS_SGN(db);
  if (gox) { 
    da_pxn = *db_px*sin(angle_n);
    db_pxn = *db_px*cos(angle_n);
  } else {
    da_pxn = *db_px*cos(angle_n);
    db_pxn = *db_px*sin(angle_n);
  }

  d_step_coef = 1;

  da_px_coef = *da_px*d_step_coef;
  db_px_coef = *db_px*d_step_coef;
  da_pxn_coef = da_pxn*d_step_coef;
  db_pxn_coef = db_pxn*d_step_coef;
  
  if(offseta != 0 || offsetb != 0) {
    if (gox) { 
      if(offseta == -99) { // old-style offset (version 3.3 and earlier)
        *acoord += MS_SGN(db)*MS_ABS(da_pxn)*offsetb;
        *bcoord -= MS_SGN(da)*MS_ABS(db_pxn)*offsetb;
      } else { // normal offset (eg. drop shadow)
        *acoord += offseta;
        *bcoord += offsetb;
      }
    } else {
      if(offsetb == -99) { // old-style offset (version 3.3 and earlier)
        *acoord -= MS_SGN(db)*MS_ABS(da_pxn)*offseta;
        *bcoord += MS_SGN(da)*MS_ABS(db_pxn)*offseta;
      } else { // normal offset (eg. drop shadow)
        *acoord += offseta;
        *bcoord += offsetb;
      }
    }
  }
  
  a = *acoord;
  b = *bcoord; 

/*  if (antialias) {
    size -= 0.5;
  } else {
    if (!da || !db) size -= 0.5;
  }*/
  
  /* Style counting */
  if (symbol->stylelength > 0) {
  
    /* Style counting unit */
    d_size = sqrt(pow(*da_px,2)+pow(*db_px,2))*d_step_coef;
    
    /* Line rendering */
    first = 1;
    for (db_line = -1*d_step_coef; db_line++ < MS_ABS(db); b += db_px_coef) {
    
      /* Set the start of the first pixel */
      an = a - da_pxn*(size-1)/2;
      bn = b - db_pxn*(size-1)/2;
    
      /* Save corners */
      if (first) {
        if (gox) {
          if (MS_SGN(db) > 0) {
            points[0].x = MS_NINT(bn);
            points[0].y = MS_NINT(an);
            points[1].x = MS_NINT(bn+db_pxn*size);
            points[1].y = MS_NINT(an+da_pxn*size);
          } else {
            points[1].x = MS_NINT(bn);
            points[1].y = MS_NINT(an);
            points[0].x = MS_NINT(bn+db_pxn*size);
            points[0].y = MS_NINT(an+da_pxn*size);
          }
          poly_points[0].x = MS_NINT(bn);
          poly_points[0].y = MS_NINT(an);
          poly_points[1].x = MS_NINT(bn+db_pxn*size);
          poly_points[1].y = MS_NINT(an+da_pxn*size);
        } else {
          if (MS_SGN(db) > 0) {
            points[0].x = MS_NINT(an);
            points[0].y = MS_NINT(bn);
            points[1].x = MS_NINT(an+da_pxn*size);
            points[1].y = MS_NINT(bn+db_pxn*size);
          } else {
            points[1].x = MS_NINT(an);
            points[1].y = MS_NINT(bn);
            points[0].x = MS_NINT(an+da_pxn*size);
            points[0].y = MS_NINT(bn+db_pxn*size);
          }
          poly_points[0].x = MS_NINT(an);
          poly_points[0].y = MS_NINT(bn);
          poly_points[1].x = MS_NINT(an+da_pxn*size);
          poly_points[1].y = MS_NINT(bn+db_pxn*size);
        }
        first = 0;
        drawpoly = *styleVis;
      }
  
      if (*styleIndex == symbol->stylelength) 
        *styleIndex = 0;
      *styleSize -= d_size;    
      if (*styleSize < 0) {
        *styleSize = symbol->style[*styleIndex]*styleCoef;
        *styleSize -= d_size;    
        (*styleIndex)++;
        *styleVis = *styleVis?0:1;
        
        if (*styleVis) {
          if (gox) {
            poly_points[0].x = MS_NINT(bn);
            poly_points[0].y = MS_NINT(an);
            poly_points[1].x = MS_NINT(bn+db_pxn*size);
            poly_points[1].y = MS_NINT(an+da_pxn*size);
          } else {
            poly_points[0].x = MS_NINT(an);
            poly_points[0].y = MS_NINT(bn);
            poly_points[1].x = MS_NINT(an+da_pxn*size);
            poly_points[1].y = MS_NINT(bn+db_pxn*size);
          }
          last_style_size = *styleSize+d_size;
          drawpoly = 1;
        } else {
          if (gox) {
            poly_points[3].x = MS_NINT(bn);
            poly_points[3].y = MS_NINT(an);
            poly_points[2].x = MS_NINT(bn+db_pxn*size);
            poly_points[2].y = MS_NINT(an+da_pxn*size);
          } else {
            poly_points[3].x = MS_NINT(an);
            poly_points[3].y = MS_NINT(bn);
            poly_points[2].x = MS_NINT(an+da_pxn*size);
            poly_points[2].y = MS_NINT(bn+db_pxn*size);
          }
          if (drawpoly) {
            if (last_style_size <= 1) {
              if (antialias) {
                //gdImageSetAntiAliased(img, c);
                gdImageSetAntiAliasedDontBlend(img, c, c);
                gdImageLine(img, poly_points[0].x, poly_points[0].y, poly_points[1].x, poly_points[1].y, gdAntiAliased);
              } else {
                gdImageLine(img, poly_points[0].x, poly_points[0].y, poly_points[1].x, poly_points[1].y, c);
              }
            } else {
              if (antialias) {
                gdImageSetAntiAliasedDontBlend(img, c, c);
                if (size > 1) {
                  gdImageFilledPolygon(img, poly_points, 4, gdAntiAliased);
                } else {
                  gdImageLine(img, poly_points[0].x, poly_points[0].y, poly_points[3].x, poly_points[3].y, gdAntiAliased);
                }
              } else {
                if (size > 1) {
                  gdImageFilledPolygon(img, poly_points, 4, c);
                } else {
                  gdImageLine(img, poly_points[0].x, poly_points[0].y, poly_points[3].x, poly_points[3].y, c);
                }
              }
            }
            drawpoly = 0;
          }
        }
      }
      
      a += da_px_coef;
    }
    
  /* Solid line - not styled */
  } else {
    /* Set the start of the first pixel */
    an = a - da_pxn*(size-1)/2;
    bn = b - db_pxn*(size-1)/2;

    if (gox) {
      if (MS_SGN(db) > 0) {
        points[0].x = MS_NINT(bn);
        points[0].y = MS_NINT(an);
        points[1].x = MS_NINT(bn+db_pxn*size);
        points[1].y = MS_NINT(an+da_pxn*size);
      } else {
        points[1].x = MS_NINT(bn);
        points[1].y = MS_NINT(an);
        points[0].x = MS_NINT(bn+db_pxn*size);
        points[0].y = MS_NINT(an+da_pxn*size);
      }
      poly_points[0].x = MS_NINT(bn);
      poly_points[0].y = MS_NINT(an);
      poly_points[1].x = MS_NINT(bn+db_pxn*size);
      poly_points[1].y = MS_NINT(an+da_pxn*size);
    } else {
      if (MS_SGN(db) > 0) {
        points[0].x = MS_NINT(an);
        points[0].y = MS_NINT(bn);
        points[1].x = MS_NINT(an+da_pxn*size);
        points[1].y = MS_NINT(bn+db_pxn*size);
      } else {
        points[1].x = MS_NINT(an);
        points[1].y = MS_NINT(bn);
        points[0].x = MS_NINT(an+da_pxn*size);
        points[0].y = MS_NINT(bn+db_pxn*size);
      }
      poly_points[0].x = MS_NINT(an);
      poly_points[0].y = MS_NINT(bn);
      poly_points[1].x = MS_NINT(an+da_pxn*size);
      poly_points[1].y = MS_NINT(bn+db_pxn*size);
    }
    drawpoly = 1;
    *styleVis = 1;
    a += da;
    b += db;
  }                   

  /* Set the start of the first pixel */
  an = a - da_pxn*(size-1)/2;
  bn = b - db_pxn*(size-1)/2;

  /* Save next corners */
  if (gox) {
    if (MS_SGN(db) > 0) {
      points[3].x = MS_NINT(bn);
      points[3].y = MS_NINT(an);
      points[2].x = MS_NINT(bn+db_pxn*size);
      points[2].y = MS_NINT(an+da_pxn*size);
    } else {
      points[2].x = MS_NINT(bn);
      points[2].y = MS_NINT(an);
      points[3].x = MS_NINT(bn+db_pxn*size);
      points[3].y = MS_NINT(an+da_pxn*size);
    }
  } else {
    if (MS_SGN(db) > 0) {
      points[3].x = MS_NINT(an);
      points[3].y = MS_NINT(bn);
      points[2].x = MS_NINT(an+da_pxn*size);
      points[2].y = MS_NINT(bn+db_pxn*size);
    } else {
      points[2].x = MS_NINT(an);
      points[2].y = MS_NINT(bn);
      points[3].x = MS_NINT(an+da_pxn*size);
      points[3].y = MS_NINT(bn+db_pxn*size);
    }
  }

  if (drawpoly) {
    if (gox) {
      poly_points[3].x = MS_NINT(bn);
      poly_points[3].y = MS_NINT(an);
      poly_points[2].x = MS_NINT(bn+db_pxn*size);
      poly_points[2].y = MS_NINT(an+da_pxn*size);
    } else {
      poly_points[3].x = MS_NINT(an);
      poly_points[3].y = MS_NINT(bn);
      poly_points[2].x = MS_NINT(an+da_pxn*size);
      poly_points[2].y = MS_NINT(bn+db_pxn*size);
    }
    
    /* Antialias */
    if (antialias) {
//      gdImageSetAntiAliased(img, c);
      gdImageSetAntiAliasedDontBlend(img, c, c);
      if (size > 1) {
        gdImageFilledPolygon(img, poly_points, 4, gdAntiAliased);
      } else {
        gdImageLine(img, poly_points[0].x, poly_points[0].y, poly_points[3].x, poly_points[3].y, gdAntiAliased);
      }
    } else {
      if (size > 1) {
        gdImageFilledPolygon(img, poly_points, 4, c);
      } else {
        gdImageLine(img, poly_points[0].x, poly_points[0].y, poly_points[3].x, poly_points[3].y, c);
      }
    }
  }
  
}

/* ------------------------------------------------------------------------------- */
/*    Draw a cartographic line symbol of the specified size, color, join and cap   */
/* ------------------------------------------------------------------------------- */
void msImageCartographicPolyline(gdImagePtr img, shapeObj *p, styleObj *style, symbolObj *symbol, int c, double size, double scalefactor)
{
  int i, j, k;
  double x, y, dx, dy, s, maxs, angle, angle_n, last_angle=0, angle_left_limit, angle_from, angle_to;

  double dy_px, dx_px, size_2;

  static int styleIndex, styleVis;
  static double styleSize=0, styleCoef=0, last_style_size=-1;
  static int last_style_c=-1, last_style_stylelength=-1, last_styleVis=0;

//  char buffer[256];

  gdPoint points[4], last_points[4];
  gdPoint cap_join_points[6];

  /* Style settings - continue with style on the next line from the same symbol */
  if (symbol->stylelength > 0 && (last_style_c != c || last_style_size != size || last_style_stylelength != symbol->stylelength)) {
    styleIndex = symbol->stylelength;
    if(style->size == -1) {
        styleCoef = size/(msSymbolGetDefaultSize(symbol));
    }
    else
        styleCoef = size/style->size;
    styleVis=0;
    last_styleVis=1;
  }
  last_style_c = c;  
  last_style_size = size;  
  last_style_stylelength = symbol->stylelength;  


  /* Draw lines */
  for (i = 0; i < p->numlines; i++) {
    for(j=1; j<p->line[i].numpoints; j++) {
      
      x = p->line[i].point[j-1].x;
      y = p->line[i].point[j-1].y;
      
      /* dx, dy */
      dx = p->line[i].point[j].x - x;
      dy = p->line[i].point[j].y - y;
      if (!dx && !dy) continue;
      
      /* A line angle */
      angle = getAngleFromDxDy(dx, dy);

      /* Normal */
      angle_n = angle - MS_PI2;
      
      /* Steps for one pixel */
      if ((MS_ABS(dx) > MS_ABS(dy)) || (dx==dy)) {
        RenderCartoLine(img, 1, &y, &x, dy, dx, angle_n, size, c, symbol, styleCoef, &styleSize, &styleIndex, &styleVis, points, &dy_px, &dx_px, style->offsety, style->offsetx, style->antialias);      
      } else {
        RenderCartoLine(img, 0, &x, &y, dx, dy, angle_n, size, c, symbol, styleCoef, &styleSize, &styleIndex, &styleVis, points, &dx_px, &dy_px, style->offsetx, style->offsety, style->antialias);      
      }

//      gdImageSetPixel(img, MS_NINT(points[0].x), MS_NINT(points[0].y), 2);

//      gdImageLine(img, p->line[i].point[j-1].x, p->line[i].point[j-1].y, p->line[i].point[j].x, p->line[i].point[j].y, 3);  

//sprintf(buffer, "styleVis: %d", styleVis);
//gdImageString(img, gdFontSmall, 10, 10, buffer, 1);

      /* Jointype */
//      if ((j > 1) && (angle != last_angle) && (last_styleVis)) {
      if (last_styleVis && (size > 3) && (j > 1) && (angle != last_angle)) {

        angle_left_limit = last_angle - MS_PI;

        switch (symbol->linejoin) {
          /* Miter */
          case MS_CJC_NONE:
            /* do nothing */  
          break;
          /* Round */
          case MS_CJC_ROUND:
            /* The current line is on the left site of the last line */ 
            if ((angle_left_limit > 0 && angle_left_limit < angle && angle < last_angle) ||
                (angle_left_limit < 0 && ((0 <= angle && angle < last_angle) || (angle_left_limit+MS_2PI < angle)))) {
              angle_from = angle_n  - MS_PI;  
              angle_to = last_angle - MS_PI2 - MS_PI;
            /* The current line is on the right site of the last line */ 
            } else {
              angle_from = last_angle - MS_PI2;  
              angle_to = angle_n;
            }
            if (angle_from > angle_to)
              angle_from -= MS_2PI;

            angle_from -= MS_DEG_TO_RAD*2;
            angle_to   += MS_DEG_TO_RAD*2;
            
            imageFilledSegment(img, x, y, (size-1)/2, angle_from, angle_to, c, style->antialias);
          break;

          /* Miter */
          case MS_CJC_MITER:
          /* Bevel */
          case MS_CJC_BEVEL:

            /* The current line is on the left site of the last line */ 
            if ((angle_left_limit > 0 && angle_left_limit < angle && angle < last_angle) ||
                (angle_left_limit < 0 && ((0 <= angle && angle < last_angle) || (angle_left_limit+MS_2PI < angle)))) {
              cap_join_points[0].x = MS_NINT(last_points[3].x); 
              cap_join_points[0].y = MS_NINT(last_points[3].y); 
              cap_join_points[2].x = MS_NINT(points[0].x); 
              cap_join_points[2].y = MS_NINT(points[0].y); 
              if ((angle == last_angle + MS_PI) || (angle == last_angle - MS_PI)) {
                cap_join_points[1].x = MS_NINT(x+dx_px*size*symbol->linejoinmaxsize);
                cap_join_points[1].y = MS_NINT(y+dy_px*size*symbol->linejoinmaxsize);
              } else { 
                cap_join_points[1] = generateGDLineIntersection(last_points[0], last_points[3], points[0], points[3]);
              } 
            /* The current line is on the right site of the last line */ 
            } else {
              cap_join_points[0].x = MS_NINT(last_points[2].x); 
              cap_join_points[0].y = MS_NINT(last_points[2].y); 
              cap_join_points[2].x = MS_NINT(points[1].x); 
              cap_join_points[2].y = MS_NINT(points[1].y); 
              if ((angle == last_angle + MS_PI) || (angle == last_angle - MS_PI)) {
                cap_join_points[1].x = MS_NINT(x-dx_px*size*symbol->linejoinmaxsize);
                cap_join_points[1].y = MS_NINT(y-dy_px*size*symbol->linejoinmaxsize);
              } else { 
                cap_join_points[1] = generateGDLineIntersection(last_points[1], last_points[2], points[1], points[2]); 
              }
            }
            /* Check the max join size */ 
            dx = cap_join_points[1].x - x; 
            dy = cap_join_points[1].y - y;
            s = sqrt(pow(dx,2)+pow(dy,2));
            
            if (symbol->linejoin == MS_CJC_MITER) {
              /* Miter */
              maxs = size*symbol->linejoinmaxsize;
              if (s > maxs) {
                s = s/maxs;
                dx = dx/s; 
                dy = dy/s;
                cap_join_points[1].x = MS_NINT(x + dx); 
                cap_join_points[1].y = MS_NINT(y + dy); 
              }
              s = s?s:1;
              cap_join_points[3].x = MS_NINT(x - dx/s); 
              cap_join_points[3].y = MS_NINT(y - dy/s); 
              ImageFilledPolygonAA(img, cap_join_points, 4, c, style->antialias);

            /* Bevel */
            } else {
              s = s?s:1;
              cap_join_points[1] = cap_join_points[2]; 
              cap_join_points[2].x = MS_NINT(x - dx/s); 
              cap_join_points[2].y = MS_NINT(y - dy/s); 
              ImageFilledPolygonAA(img, cap_join_points, 3, c, style->antialias);
            }
          break;
        }
      }

      /* Captype */
      if (symbol->linecap != MS_CJC_BUTT && size > 3 && ((j == 1) || (j == (p->line[i].numpoints-1)))) { 
        size_2 = size/sqrt(pow(dx_px,2)+pow(dy_px,2))/2;
        switch (symbol->linecap) {
          /* Butt */
          case MS_CJC_BUTT:
            /* do nothing */
          break;
          /* Round */
          case MS_CJC_ROUND:
            /* First point */            
            if (j == 1) { 
              dx = points[0].x - points[1].x;
              dy = points[0].y - points[1].y;
              angle_from = angle + MS_PI2;
              angle_from -= angle_from > MS_2PI?MS_2PI:0;
              angle_to   = angle_from + MS_PI;
              s = sqrt(pow(dx,2) + pow(dy,2))/2;
              imageFilledSegment(img, points[0].x+dx_px-dx/2.0, points[0].y+dy_px-dy/2.0, s, angle_from, angle_to, c, style->antialias);
            } 
            /* Last point */            
            if (j == (p->line[i].numpoints-1)) {
              dx = points[2].x - points[3].x;
              dy = points[2].y - points[3].y;
              angle_from = angle - MS_PI2;
              angle_from += angle_from < 0?MS_2PI:0;
              angle_to   = angle_from + MS_PI;
              s = sqrt(pow(dx,2) + pow(dy,2))/2;
              imageFilledSegment(img, points[2].x-dx_px-dx/2.0, points[2].y-dy_px-dy/2.0, s, angle_from, angle_to, c, style->antialias);
            }
          break;
          /* Square */
          case MS_CJC_SQUARE:
            /* First point */            
            if (j == 1) { 
              cap_join_points[0].x = MS_NINT(points[0].x+dx_px); 
              cap_join_points[0].y = MS_NINT(points[0].y+dy_px); 
              cap_join_points[1].x = MS_NINT(points[1].x+dx_px); 
              cap_join_points[1].y = MS_NINT(points[1].y+dy_px); 
              cap_join_points[2].x = MS_NINT(points[1].x - size_2*dx_px); 
              cap_join_points[2].y = MS_NINT(points[1].y - size_2*dy_px); 
              cap_join_points[3].x = MS_NINT(points[0].x - size_2*dx_px); 
              cap_join_points[3].y = MS_NINT(points[0].y - size_2*dy_px);
              ImageFilledPolygonAA(img, cap_join_points, 4, c, style->antialias);
            } 
            /* Last point */            
            if (j == (p->line[i].numpoints-1)) {
              cap_join_points[0].x = MS_NINT(points[2].x-dx_px); 
              cap_join_points[0].y = MS_NINT(points[2].y-dy_px); 
              cap_join_points[1].x = MS_NINT(points[3].x-dx_px); 
              cap_join_points[1].y = MS_NINT(points[3].y-dy_px); 
              cap_join_points[2].x = MS_NINT(points[3].x + size_2*dx_px); 
              cap_join_points[2].y = MS_NINT(points[3].y + size_2*dy_px); 
              cap_join_points[3].x = MS_NINT(points[2].x + size_2*dx_px); 
              cap_join_points[3].y = MS_NINT(points[2].y + size_2*dy_px);
              ImageFilledPolygonAA(img, cap_join_points, 4, c, style->antialias);
            } 
          break;
          /* Triangle */
          case MS_CJC_TRIANGLE:
            /* First point */            
            if (j == 1) { 
              cap_join_points[0].x = MS_NINT(points[0].x+dx_px); 
              cap_join_points[0].y = MS_NINT(points[0].y+dy_px); 
              cap_join_points[1].x = MS_NINT(points[1].x+dx_px); 
              cap_join_points[1].y = MS_NINT(points[1].y+dy_px); 
              cap_join_points[2].x = MS_NINT((points[0].x+points[1].x)/2 - size_2*dx_px); 
              cap_join_points[2].y = MS_NINT((points[0].y+points[1].y)/2 - size_2*dy_px); 
              ImageFilledPolygonAA(img, cap_join_points, 3, c, style->antialias);
            } 
            /* Last point */            
            if (j == (p->line[i].numpoints-1)) {
              cap_join_points[0].x = MS_NINT(points[2].x-dx_px); 
              cap_join_points[0].y = MS_NINT(points[2].y-dy_px); 
              cap_join_points[1].x = MS_NINT(points[3].x-dx_px); 
              cap_join_points[1].y = MS_NINT(points[3].y-dy_px); 
              cap_join_points[2].x = MS_NINT((points[2].x+points[3].x)/2 + size_2*dx_px); 
              cap_join_points[2].y = MS_NINT((points[2].y+points[3].y)/2 + size_2*dy_px); 
              ImageFilledPolygonAA(img, cap_join_points, 3, c, style->antialias);
            } 
          break;
        }
      }  
      
/*      gdImageLine(img, points[0].x, points[0].y, points[1].x, points[1].y, 3);  
      gdImageLine(img, points[1].x, points[1].y, points[2].x, points[2].y, 3);  
      gdImageLine(img, points[2].x, points[2].y, points[3].x, points[3].y, 3);  
*/
      /* Copy the last point and angle */
      for (k=0; k<4; k++) {
        last_points[k] = points[k];
//        gdImageSetPixel(img, MS_NINT(points[k].x), MS_NINT(points[k].y), 1); 
      }
      last_angle = angle;
      last_styleVis = styleVis;
    }

  }
}

static void billboardGD(gdImagePtr img, shapeObj *shape, labelObj *label)
{
  int i;
  shapeObj temp;

  msInitShape(&temp);
  msAddLine(&temp, &shape->line[0]);

  if(label->backgroundcolor.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(label->backgroundcolor));
  if(label->backgroundshadowcolor.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(label->backgroundshadowcolor));

  if(label->backgroundshadowcolor.pen >= 0) {
    for(i=0; i<temp.line[0].numpoints; i++) {
      temp.line[0].point[i].x += label->backgroundshadowsizex;
      temp.line[0].point[i].y += label->backgroundshadowsizey;
    }
    imageFilledPolygon(img, &temp, label->backgroundshadowcolor.pen, 0, 0);
    for(i=0; i<temp.line[0].numpoints; i++) {
      temp.line[0].point[i].x -= label->backgroundshadowsizex;
      temp.line[0].point[i].y -= label->backgroundshadowsizey;
    }
  }

  imageFilledPolygon(img, &temp, label->backgroundcolor.pen, 0, 0);

  msFreeShape(&temp);
}

/*
** Simple charset converter.
** The return value must be freed by the caller.
*/
char *msGetEncodedString(const char *string, const char *encoding)
{
#ifdef USE_ICONV
  iconv_t cd = NULL;
  char *in, *inp;
  char *outp, *out = NULL;
  size_t len, bufsize, bufleft, status;

  cd = iconv_open("UTF-8", encoding);
  if(cd == (iconv_t)-1) {
    msSetError(MS_IDENTERR, "Encoding not support by libiconv (%s).", 
               "msGetEncodedString()", encoding);
    return NULL;
  }

  len = strlen(string);
  bufsize = len * 4;
  in = strdup(string);
  inp = in;
  out = (char*) malloc(bufsize);
  if(in == NULL || out == NULL){
    msSetError(MS_MEMERR, NULL, "msGetEncodedString()");
    msFree(in);
    iconv_close(cd);
    return NULL;
  }
  strcpy(out, in);
  outp = out;

  bufleft = bufsize;
  status = -1;

  while (len > 0){
    status = iconv(cd, &inp, &len, &outp, &bufleft);
    if(status == -1){
      msFree(in);
      msFree(out);
      iconv_close(cd);
      return strdup(string);
    }
  }
  out[bufsize - bufleft] = '\0';
  
  msFree(in);
  iconv_close(cd);

  return out;
#else
  msSetError(MS_MISCERR, "Not implemeted since Iconv is not enabled.", 
             "msGetEncodedString()");
  return NULL;
#endif
}

/*
** Simply draws a label based on the label point and the supplied label object.
*/
int msDrawTextGD(gdImagePtr img, pointObj labelPnt, char *string, labelObj *label, fontSetObj *fontset, double scalefactor)
{
  int x, y;
  int oldAlphaBlending=0;

  if(!string) return(0); /* not errors, just don't want to do anything */
  if(strlen(string) == 0) return(0);

  if( label->encoding != NULL ) { /* converting the label encoding */
      string = msGetEncodedString(string, label->encoding);
      if(string == NULL) return(-1);
  }
  x = MS_NINT(labelPnt.x);
  y = MS_NINT(labelPnt.y);

  if(label->color.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(label->color));
  if(label->outlinecolor.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(label->outlinecolor));
  if(label->shadowcolor.pen == MS_PEN_UNSET) msImageSetPenGD(img, &(label->shadowcolor));

  if(label->type == MS_TRUETYPE) {
    char *error=NULL, *font=NULL;
    int bbox[8];
    double angle_radians = MS_DEG_TO_RAD*label->angle;
    double size;

    size = label->size*scalefactor;
    size = MS_MAX(size, label->minsize);
    size = MS_MIN(size, label->maxsize);

#ifdef USE_GD_FT
    if(!fontset) {
      msSetError(MS_TTFERR, "No fontset defined.", "msDrawTextGD()");
      if(label->encoding != NULL) msFree(string);
      return(-1);
    }

    if(!label->font) {
      msSetError(MS_TTFERR, "No Trueype font defined.", "msDrawTextGD()");
      if(label->encoding != NULL) msFree(string);
      return(-1);
    }

    font = msLookupHashTable(&(fontset->fonts), label->font);
    if(!font) {
      msSetError(MS_TTFERR, "Requested font (%s) not found.", "msDrawTextGD()", label->font);
      if(label->encoding != NULL) msFree(string);
      return(-1);
    }

    if( gdImageTrueColor(img) )
    {
        oldAlphaBlending = img->alphaBlendingFlag;
        gdImageAlphaBlending( img, 1 );
    }

    if(label->outlinecolor.pen >= 0) { /* handle the outline color */
      error = gdImageStringFT(img, bbox, ((label->antialias)?(label->outlinecolor.pen):-(label->outlinecolor.pen)), font, size, angle_radians, x, y-1, string);
      if(error) {
        if( gdImageTrueColor(img) )
            gdImageAlphaBlending( img, oldAlphaBlending );
	msSetError(MS_TTFERR, error, "msDrawTextGD()");
        if(label->encoding != NULL) msFree(string);
	return(-1);
      }

      gdImageStringFT(img, bbox, ((label->antialias)?(label->outlinecolor.pen):-(label->outlinecolor.pen)), font, size, angle_radians, x, y+1, string);
      gdImageStringFT(img, bbox, ((label->antialias)?(label->outlinecolor.pen):-(label->outlinecolor.pen)), font, size, angle_radians, x+1, y, string);
      gdImageStringFT(img, bbox, ((label->antialias)?(label->outlinecolor.pen):-(label->outlinecolor.pen)), font, size, angle_radians, x-1, y, string);
      
      gdImageStringFT(img, bbox, ((label->antialias)?(label->outlinecolor.pen):-(label->outlinecolor.pen)), font, size, angle_radians, x-1, y-1, string);      
      gdImageStringFT(img, bbox, ((label->antialias)?(label->outlinecolor.pen):-(label->outlinecolor.pen)), font, size, angle_radians, x-1, y+1, string);
      gdImageStringFT(img, bbox, ((label->antialias)?(label->outlinecolor.pen):-(label->outlinecolor.pen)), font, size, angle_radians, x+1, y-1, string);
      gdImageStringFT(img, bbox, ((label->antialias)?(label->outlinecolor.pen):-(label->outlinecolor.pen)), font, size, angle_radians, x+1, y+1, string);
    }

    if(label->shadowcolor.pen >= 0) { /* handle the shadow color */
      error = gdImageStringFT(img, bbox, ((label->antialias)?(label->shadowcolor.pen):-(label->shadowcolor.pen)), font, size, angle_radians, x+label->shadowsizex, y+label->shadowsizey, string);
      if(error) {
	msSetError(MS_TTFERR, error, "msDrawTextGD()");
        if(label->encoding != NULL) msFree(string);
	return(-1);
      }
    }

    gdImageStringFT(img, bbox, ((label->antialias)?(label->color.pen):-(label->color.pen)), font, size, angle_radians, x, y, string);

    if( gdImageTrueColor(img) )
        gdImageAlphaBlending( img, oldAlphaBlending );

#else
    msSetError(MS_TTFERR, "TrueType font support is not available.", "msDrawTextGD()");
    if(label->encoding != NULL) msFree(string);
    return(-1);
#endif

  } else { /* MS_BITMAP */
    char **token=NULL;
    int t, num_tokens;
    gdFontPtr fontPtr;

    if((fontPtr = msGetBitmapFont(label->size)) == NULL) {
      if(label->encoding != NULL) msFree(string);
      return(-1);
    }

    if(label->wrap != '\0') {
      if((token = split(string, label->wrap, &(num_tokens))) == NULL) {
        if(label->encoding != NULL) msFree(string);
	return(-1);
      }

      y -= fontPtr->h*num_tokens;
      for(t=0; t<num_tokens; t++) {
	if(label->outlinecolor.pen >= 0) {	  
	  gdImageString(img, fontPtr, x, y-1, token[t], label->outlinecolor.pen);
	  gdImageString(img, fontPtr, x, y+1, token[t], label->outlinecolor.pen);
	  gdImageString(img, fontPtr, x+1, y, token[t], label->outlinecolor.pen);
	  gdImageString(img, fontPtr, x-1, y, token[t], label->outlinecolor.pen);
	  gdImageString(img, fontPtr, x+1, y-1, token[t], label->outlinecolor.pen);
	  gdImageString(img, fontPtr, x+1, y+1, token[t], label->outlinecolor.pen);
	  gdImageString(img, fontPtr, x-1, y-1, token[t], label->outlinecolor.pen);
	  gdImageString(img, fontPtr, x-1, y+1, token[t], label->outlinecolor.pen);
	}

	if(label->shadowcolor.pen >= 0)
	  gdImageString(img, fontPtr, x+label->shadowsizex, y+label->shadowsizey, token[t], label->shadowcolor.pen);

	gdImageString(img, fontPtr, x, y, token[t], label->color.pen);

	y += fontPtr->h; /* shift down */
      }

      msFreeCharArray(token, num_tokens);
    } else {
      y -= fontPtr->h;

      if(label->outlinecolor.pen >= 0) {
	gdImageString(img, fontPtr, x, y-1, string, label->outlinecolor.pen);
	gdImageString(img, fontPtr, x, y+1, string, label->outlinecolor.pen);
	gdImageString(img, fontPtr, x+1, y, string, label->outlinecolor.pen);
	gdImageString(img, fontPtr, x-1, y, string, label->outlinecolor.pen);
	gdImageString(img, fontPtr, x+1, y-1, string, label->outlinecolor.pen);
	gdImageString(img, fontPtr, x+1, y+1, string, label->outlinecolor.pen);
	gdImageString(img, fontPtr, x-1, y-1, string, label->outlinecolor.pen);
	gdImageString(img, fontPtr, x-1, y+1, string, label->outlinecolor.pen);
      }

      if(label->shadowcolor.pen >= 0)
	gdImageString(img, fontPtr, x+label->shadowsizex, y+label->shadowsizey, string, label->shadowcolor.pen);

      gdImageString(img, fontPtr, x, y, string, label->color.pen);
    }
  }

  if(label->encoding != NULL) msFree(string);

  return(0);
}

// TO DO: fix returned values to be MS_SUCCESS/MS_FAILURE
int msDrawLabelCacheGD(gdImagePtr img, mapObj *map)
{
  pointObj p;
  int i, j, l;
  int oldAlphaBlending=0;
  rectObj r;
  
  labelCacheMemberObj *cachePtr=NULL;
  layerObj *layerPtr=NULL;
  labelObj *labelPtr=NULL;

  int marker_width, marker_height;
  int marker_offset_x, marker_offset_y;
  rectObj marker_rect;

  /* bug 490 - switch on alpha blending for label cache */
  oldAlphaBlending = img->alphaBlendingFlag;
  gdImageAlphaBlending( img, 1);
  
  for(l=map->labelcache.numlabels-1; l>=0; l--) {

    cachePtr = &(map->labelcache.labels[l]); // point to right spot in the label cache

    layerPtr = &(map->layers[cachePtr->layerindex]); // set a couple of other pointers, avoids nasty references
    labelPtr = &(cachePtr->label);

    if(!cachePtr->text || strlen(cachePtr->text) == 0)
      continue; // not an error, just don't want to do anything

    if(msGetLabelSize(cachePtr->text, labelPtr, &r, &(map->fontset), layerPtr->scalefactor) == -1)
      return(-1);

    if(labelPtr->autominfeaturesize && ((r.maxx-r.minx) > cachePtr->featuresize))
      continue; /* label too large relative to the feature */

    marker_offset_x = marker_offset_y = 0; /* assume no marker */
    if((layerPtr->type == MS_LAYER_ANNOTATION && cachePtr->numstyles > 0) || layerPtr->type == MS_LAYER_POINT) { // there *is* a marker      

      // TO DO: at the moment only checks the bottom style, perhaps should check all of them
      if(msGetMarkerSize(&map->symbolset, &(cachePtr->styles[0]), &marker_width, &marker_height, layerPtr->scalefactor) != MS_SUCCESS)
	return(-1);

      marker_offset_x = MS_NINT(marker_width/2.0);
      marker_offset_y = MS_NINT(marker_height/2.0);      

      marker_rect.minx = MS_NINT(cachePtr->point.x - .5 * marker_width);
      marker_rect.miny = MS_NINT(cachePtr->point.y - .5 * marker_height);
      marker_rect.maxx = marker_rect.minx + (marker_width-1);
      marker_rect.maxy = marker_rect.miny + (marker_height-1); 
    }

    if(labelPtr->position == MS_AUTO) {

      if(layerPtr->type == MS_LAYER_LINE) {
	int position = MS_UC;

	for(j=0; j<2; j++) { /* Two angles or two positions, depending on angle. Steep angles will use the angle approach, otherwise we'll rotate between UC and LC. */

	  msFreeShape(cachePtr->poly);
	  cachePtr->status = MS_TRUE; /* assume label *can* be drawn */

	  if(j == 1) {
	    if(fabs(cos(labelPtr->angle)) < LINE_VERT_THRESHOLD)
	      labelPtr->angle += 180.0;
	    else
	      position = MS_LC;
	  }

	  p = get_metrics(&(cachePtr->point), position, r, (marker_offset_x + labelPtr->offsetx), (marker_offset_y + labelPtr->offsety), labelPtr->angle, labelPtr->buffer, cachePtr->poly);

	  if(layerPtr->type == MS_LAYER_ANNOTATION && cachePtr->numstyles > 0)
	    msRectToPolygon(marker_rect, cachePtr->poly); // save marker bounding polygon

	  if(!labelPtr->partials) { // check against image first
	    if(labelInImage(img->sx, img->sy, cachePtr->poly, labelPtr->buffer) == MS_FALSE) {
	      cachePtr->status = MS_FALSE;
	      continue; // next angle
	    }
	  }

	  for(i=0; i<map->labelcache.nummarkers; i++) { // compare against points already drawn
	    if(l != map->labelcache.markers[i].id) { // labels can overlap their own marker
	      if(intersectLabelPolygons(map->labelcache.markers[i].poly, cachePtr->poly) == MS_TRUE) { /* polys intersect */
		cachePtr->status = MS_FALSE;
		break;
	      }
	    }
	  }

	  if(!cachePtr->status)
	    continue; // next angle

	  for(i=l+1; i<map->labelcache.numlabels; i++) { // compare against rendered labels
	    if(map->labelcache.labels[i].status == MS_TRUE) { /* compare bounding polygons and check for duplicates */

	      if((labelPtr->mindistance != -1) && (cachePtr->classindex == map->labelcache.labels[i].classindex) && (strcmp(cachePtr->text,map->labelcache.labels[i].text) == 0) && (msDistancePointToPoint(&(cachePtr->point), &(map->labelcache.labels[i].point)) <= labelPtr->mindistance)) { /* label is a duplicate */
		cachePtr->status = MS_FALSE;
		break;
	      }

	      if(intersectLabelPolygons(map->labelcache.labels[i].poly, cachePtr->poly) == MS_TRUE) { /* polys intersect */
		cachePtr->status = MS_FALSE;
		break;
	      }
	    }
	  }

	  if(cachePtr->status) // found a suitable place for this label
	    break;

	} // next angle

      } else {
	for(j=0; j<=7; j++) { /* loop through the outer label positions */

	  msFreeShape(cachePtr->poly);
	  cachePtr->status = MS_TRUE; /* assume label *can* be drawn */

	  p = get_metrics(&(cachePtr->point), j, r, (marker_offset_x + labelPtr->offsetx), (marker_offset_y + labelPtr->offsety), labelPtr->angle, labelPtr->buffer, cachePtr->poly);

	  if(layerPtr->type == MS_LAYER_ANNOTATION && cachePtr->numstyles > 0)
	    msRectToPolygon(marker_rect, cachePtr->poly); // save marker bounding polygon

	  if(!labelPtr->partials) { // check against image first
	    if(labelInImage(img->sx, img->sy, cachePtr->poly, labelPtr->buffer) == MS_FALSE) {
	      cachePtr->status = MS_FALSE;
	      continue; // next position
	    }
	  }

	  for(i=0; i<map->labelcache.nummarkers; i++) { // compare against points already drawn
	    if(l != map->labelcache.markers[i].id) { // labels can overlap their own marker
	      if(intersectLabelPolygons(map->labelcache.markers[i].poly, cachePtr->poly) == MS_TRUE) { /* polys intersect */
		cachePtr->status = MS_FALSE;
		break;
	      }
	    }
	  }

	  if(!cachePtr->status)
	    continue; // next position

	  for(i=l+1; i<map->labelcache.numlabels; i++) { // compare against rendered labels
	    if(map->labelcache.labels[i].status == MS_TRUE) { /* compare bounding polygons and check for duplicates */

	      if((labelPtr->mindistance != -1) && (cachePtr->classindex == map->labelcache.labels[i].classindex) && (strcmp(cachePtr->text,map->labelcache.labels[i].text) == 0) && (msDistancePointToPoint(&(cachePtr->point), &(map->labelcache.labels[i].point)) <= labelPtr->mindistance)) { /* label is a duplicate */
		cachePtr->status = MS_FALSE;
		break;
	      }

	      if(intersectLabelPolygons(map->labelcache.labels[i].poly, cachePtr->poly) == MS_TRUE) { /* polys intersect */
		cachePtr->status = MS_FALSE;
		break;
	      }
	    }
	  }

	  if(cachePtr->status) // found a suitable place for this label
	    break;
	} // next position
      }

      if(labelPtr->force) cachePtr->status = MS_TRUE; /* draw in spite of collisions based on last position, need a *best* position */

    } else {
      cachePtr->status = MS_TRUE; /* assume label *can* be drawn */

      if(labelPtr->position == MS_CC) // don't need the marker_offset
        p = get_metrics(&(cachePtr->point), labelPtr->position, r, labelPtr->offsetx, labelPtr->offsety, labelPtr->angle, labelPtr->buffer, cachePtr->poly);
      else
        p = get_metrics(&(cachePtr->point), labelPtr->position, r, (marker_offset_x + labelPtr->offsetx), (marker_offset_y + labelPtr->offsety), labelPtr->angle, labelPtr->buffer, cachePtr->poly);

      if(layerPtr->type == MS_LAYER_ANNOTATION && cachePtr->numstyles > 0)
	msRectToPolygon(marker_rect, cachePtr->poly); /* save marker bounding polygon, part of overlap tests */

      if(!labelPtr->force) { // no need to check anything else

	if(!labelPtr->partials) {
	  if(labelInImage(img->sx, img->sy, cachePtr->poly, labelPtr->buffer) == MS_FALSE)
	    cachePtr->status = MS_FALSE;
	}

	if(!cachePtr->status)
	  continue; // next label

	for(i=0; i<map->labelcache.nummarkers; i++) { // compare against points already drawn
	  if(l != map->labelcache.markers[i].id) { // labels can overlap their own marker
	    if(intersectLabelPolygons(map->labelcache.markers[i].poly, cachePtr->poly) == MS_TRUE) { /* polys intersect */
	      cachePtr->status = MS_FALSE;
	      break;
	    }
	  }
	}

	if(!cachePtr->status)
	  continue; // next label

	for(i=l+1; i<map->labelcache.numlabels; i++) { // compare against rendered label
	  if(map->labelcache.labels[i].status == MS_TRUE) { /* compare bounding polygons and check for duplicates */
	    if((labelPtr->mindistance != -1) && (cachePtr->classindex == map->labelcache.labels[i].classindex) && (strcmp(cachePtr->text, map->labelcache.labels[i].text) == 0) && (msDistancePointToPoint(&(cachePtr->point), &(map->labelcache.labels[i].point)) <= labelPtr->mindistance)) { /* label is a duplicate */
	      cachePtr->status = MS_FALSE;
	      break;
	    }

	    if(intersectLabelPolygons(map->labelcache.labels[i].poly, cachePtr->poly) == MS_TRUE) { /* polys intersect */          
	      cachePtr->status = MS_FALSE;
	      break;
	    }
	  }
	}
      }
    } /* end position if-then-else */

    /* imagePolyline(img, cachePtr->poly, 1, 0, 0); */

    if(!cachePtr->status)
      continue; /* next label */

    if(layerPtr->type == MS_LAYER_ANNOTATION && cachePtr->numstyles > 0) { /* need to draw a marker */
      for(i=0; i<cachePtr->numstyles; i++)
        msDrawMarkerSymbolGD(&map->symbolset, img, &(cachePtr->point), &(cachePtr->styles[i]), layerPtr->scalefactor);
    }

    if(MS_VALID_COLOR(labelPtr->backgroundcolor)) billboardGD(img, cachePtr->poly, labelPtr);
    msDrawTextGD(img, p, cachePtr->text, labelPtr, &(map->fontset), layerPtr->scalefactor); // actually draw the label

  } // next label

  /* bug 490 - alpha blending back */
  gdImageAlphaBlending( img, oldAlphaBlending);
  
  return(0);
}

/* ===========================================================================
   msSaveImageGD
   
   Save an image to a file named filename, if filename is NULL it goes to
   stdout.  This function wraps msSaveImageStreamGD.  --SG
   ======================================================================== */
int msSaveImageGD( gdImagePtr img, char *filename, outputFormatObj *format )
{
    FILE *stream;
    int retval=MS_FAILURE;

    /* Try to open a file handle */
    if (filename != NULL && strlen(filename) > 0)
    {
        stream = fopen(filename, "wb");
        if (!stream)
        {
            msSetError(MS_IOERR, "Unable to open file %s for writing",
                       "msSaveImageGD()", filename);
            return MS_FAILURE;
        }
    
        retval = msSaveImageStreamGD( img, stream, format );
        fclose(stream);
    } 
    /* Fall back on standard output, or MAPIO's replacement */
    else 
    {
        return msSaveImageStreamGD(img, NULL, format);
    }

    return retval;
}

/* ===========================================================================
   msSaveImageStreamGD

   Save image data to an open file variable, or if this NULL, stdout.
   ======================================================================== */
int msSaveImageStreamGD( gdImagePtr img, FILE *file, outputFormatObj *format)
{
  FILE *stream;
  gdIOCtx *gd_ctx = NULL;

  if( format->imagemode == MS_IMAGEMODE_RGBA )
    gdImageSaveAlpha( img, 1 );
  else if( format->imagemode == MS_IMAGEMODE_RGB )
    gdImageSaveAlpha( img, 0 );

  /* first, try output to file */
  if (file)
    stream = file;
  /* else use standard output, or mapio's replacement for stdout */
  else 
  {

#if defined(_WIN32) && !defined(USE_FASTCGI)
    /*
    ** Change stdout mode to binary on win32 platforms
    **
    ** --
    ** 
    ** But don't do it we are using FastCGI.  We will take care of doing
    ** it in the libfcgi library in that case for the normal cgi case, and
    ** for the fastcgi case the _setmode() call causes a crash. 
    **
    ** In general I think we should move this out so we don't have to 
    ** replicate it everwhere we produce binary output. FrankWarmerdam/2004
    */
    if(_setmode( _fileno(stdout), _O_BINARY) == -1) {
      msSetError(MS_IOERR, "Unable to change stdout to binary mode.", "msSaveImage()");
      return(MS_FAILURE);
    }
#endif
    stream = stdout;
#ifdef USE_MAPIO
    gd_ctx = msIO_getGDIOCtx( stream );
#endif
  }

  if( strcasecmp("ON",msGetOutputFormatOption( format, "INTERLACE", "ON" ))
      == 0 )
      gdImageInterlace(img, 1);

  if(format->transparent)
    gdImageColorTransparent(img, 0);

  if( strcasecmp(format->driver,"gd/gif") == 0 )
  {
#ifdef USE_GD_GIF
      if( gd_ctx != NULL )
          gdImageGifCtx( img, gd_ctx );
      else
          gdImageGif(img, stream);
#else
    msSetError(MS_MISCERR, "GIF output is not available.", "msSaveImage()");
    return(MS_FAILURE);
#endif
  }
  else if( strcasecmp(format->driver,"gd/png") == 0 )
  {
#ifdef USE_GD_PNG
      if( gd_ctx != NULL )
          gdImagePngCtx( img, gd_ctx );
      else
          gdImagePng(img, stream);
#else
      msSetError(MS_MISCERR, "PNG output is not available.", "msSaveImage()");
      return(MS_FAILURE);
#endif
  }
  else if( strcasecmp(format->driver,"gd/jpeg") == 0 )
  {
#ifdef USE_GD_JPEG
      if( gd_ctx != NULL )
          gdImageJpegCtx(img, gd_ctx, 
                      atoi(msGetOutputFormatOption( format, "QUALITY", "75")));
      else
          gdImageJpeg(img, stream, 
                      atoi(msGetOutputFormatOption( format, "QUALITY", "75")));
#else
     msSetError(MS_MISCERR, "JPEG output is not available.", "msSaveImage()");
     return(MS_FAILURE);
#endif
  }
  else if( strcasecmp(format->driver,"gd/wbmp") == 0 )
  {
#ifdef USE_GD_WBMP
      if( gd_ctx != NULL )
          gdImageWBMPCtx(img, 1, gd_ctx);
      else
          gdImageWBMP(img, 1, stream);
#else
      msSetError(MS_MISCERR, "WBMP output is not available.", "msSaveImage()");
      return(MS_FAILURE);
#endif
  }
  else
  {
      msSetError(MS_MISCERR, "Unknown output image type driver: %s.", 
                 "msSaveImage()", format->driver );
      return(MS_FAILURE);
  }

  if( gd_ctx != NULL )
      free( gd_ctx );

  return(MS_SUCCESS);
}

/* ===========================================================================
   msSaveImageBufferGD

   Save image data to a unsigned char * buffer.  In the future we should try
   to merge this with msSaveImageStreamGD function.
   ======================================================================== */

unsigned char *msSaveImageBufferGD(gdImagePtr img, int *size_ptr, outputFormatObj *format)
{
    unsigned char *imgbytes;
    
    if (format->imagemode == MS_IMAGEMODE_RGBA) 
        gdImageSaveAlpha(img, 1);
    else if (format->imagemode == MS_IMAGEMODE_RGB)
        gdImageSaveAlpha(img, 0);

    if (strcasecmp("ON", 
        msGetOutputFormatOption(format, "INTERLACE", "ON" )) == 0) 
        gdImageInterlace(img, 1);

    if (format->transparent)
        gdImageColorTransparent(img, 0);

    if (strcasecmp(format->driver, "gd/gif") == 0) {
#ifdef USE_GD_GIF
        imgbytes = gdImageGifPtr(img, size_ptr);
#else
        msSetError(MS_IMGERR, "GIF output is not available.", 
                   "msSaveImageBufferGD()");
        return NULL;
#endif
    } else if (strcasecmp(format->driver, "gd/png") == 0) {
#ifdef USE_GD_PNG
        imgbytes = gdImagePngPtr(img, size_ptr);
#else
        msSetError(MS_IMGERR, "PNG output is not available.", 
                   "msSaveImageBufferGD()");
        return NULL;
#endif
    } else if (strcasecmp(format->driver, "gd/jpeg") == 0) {
#ifdef USE_GD_JPEG
        imgbytes = gdImageJpegPtr(img, size_ptr, 
            atoi(msGetOutputFormatOption(format, "QUALITY", "75" )));
#else
        msSetError(MS_IMGERR, "JPEG output is not available.", 
                   "msSaveImageBufferGD()");
        return NULL;
#endif
    } else if (strcasecmp(format->driver, "gd/wbmp") == 0) {
#ifdef USE_GD_WBMP
        imgbytes = gdImageWBMPPtr(img, size_ptr, 1);
#else
        msSetError(MS_IMGERR, "WBMP output is not available.", 
                  "msSaveImageBufferGD()");
        return NULL;
#endif
    } else {
        msSetError(MS_IMGERR, "Unknown output image type driver: %s.", 
                   "msSaveImageBufferGD()", format->driver );
        return NULL;
    } 

    return imgbytes;
}


/**
 * Free gdImagePtr
 */
void msFreeImageGD(gdImagePtr img)
{
  gdImageDestroy(img);
}

static void msFixedImageCopy (gdImagePtr dst, gdImagePtr src,  int dstX, int dstY, int srcX, int srcY, int w, int h)
{
    int x, y;

    /* for most cases the GD copy is fine */
    if( !gdImageTrueColor(dst) || gdImageTrueColor(src) )
        return gdImageCopy( dst, src, dstX, dstY, srcX, srcY, w, h );

    /* But for copying 8bit to 24bit the GD 2.0.1 copy has a bug with
       transparency */

    for (y = 0; (y < h); y++)
    {
        for (x = 0; (x < w); x++)
        {
            int c_8 = gdImageGetPixel (src, srcX + x, srcY + y);

            if (c_8 != src->transparent)
            {
                int c;

                c = gdTrueColorAlpha (src->red[c_8], 
                                      src->green[c_8], 
                                      src->blue[c_8],
                                      gdAlphaOpaque );
                gdImageSetPixel (dst,
                                 dstX + x,
                                 dstY + y,
                                 c);
            }
        }
    }
    return;
}

void msImageCopyMerge (gdImagePtr dst, gdImagePtr src, int dstX, int dstY, int srcX, int srcY, int w, int h, int pct)
{
    int x, y;
    int oldAlphaBlending=0;

    /* for most cases the GD copy is fine */
    if( !gdImageTrueColor(dst) || !gdImageTrueColor(src) )
        return gdImageCopyMerge( dst, src, dstX, dstY, srcX, srcY, w, h, pct );

    /* 
    ** Turn off blending in output image to prevent it doing it's own attempt
    ** at blending instead of using our result. 
    */

    oldAlphaBlending = dst->alphaBlendingFlag;
    gdImageAlphaBlending( dst, 0 );

    //gdImageAlphaBlending( dst, 0 );

    for (y = 0; (y < h); y++)
    {
        for (x = 0; (x < w); x++)
        {
            int src_c = gdImageGetPixel (src, srcX + x, srcY + y);
            int dst_c = gdImageGetPixel (dst, dstX + x, dstY + y);
            int red, green, blue, res_alpha;
            int src_alpha = (127-gdTrueColorGetAlpha(src_c));
            int dst_alpha = (127-gdTrueColorGetAlpha(dst_c));

            if( gdTrueColorGetAlpha(src_c) == gdAlphaTransparent )
                continue;

            /* Adjust dst alpha according to percentages */
            dst_alpha = dst_alpha * ((100-pct)*src_alpha/127) / 100;

            /* adjust source according to transparency percentage */
            src_alpha = src_alpha * (pct) / 100;

            /* Use simple additive model for resulting transparency */
            res_alpha = src_alpha + dst_alpha;
            if( res_alpha > 127 )
                res_alpha = 127;
            
            if( src_alpha + dst_alpha == 0 )
                dst_alpha = 1;

            red = ((gdTrueColorGetRed( src_c ) * src_alpha)
                   + (gdTrueColorGetRed( dst_c ) * dst_alpha))
                / (src_alpha+dst_alpha);
            green = ((gdTrueColorGetGreen( src_c ) * src_alpha)
                     + (gdTrueColorGetGreen( dst_c ) * dst_alpha))
                / (src_alpha+dst_alpha);
            blue = ((gdTrueColorGetBlue( src_c ) * src_alpha)
                    + (gdTrueColorGetBlue( dst_c ) * dst_alpha))
                / (src_alpha+dst_alpha);
            
            gdImageSetPixel(dst,dstX+x,dstY+y,
                            gdTrueColorAlpha( red, green, blue, 
                                              127-res_alpha ));
        }
    }

    /*
    ** Restore original alpha blending flag. 
    */
    //gdImageAlphaBlending( dst, 0 );
    gdImageAlphaBlending( dst, oldAlphaBlending );
}
