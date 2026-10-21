#include "native.h"
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

bool bspp_verbose = false;

static void print_runtime_summary(const struct timespec *start) {
    if (!bspp_verbose) return;
    struct timespec end;
    struct rusage end_usage;
    if (clock_gettime(CLOCK_MONOTONIC, &end)) return;
    if (getrusage(RUSAGE_SELF, &end_usage)) return;

    double elapsed = (double)(end.tv_sec - start->tv_sec) +
                    (double)(end.tv_nsec - start->tv_nsec) / 1000000000.0;
    size_t peak_bytes = (size_t)end_usage.ru_maxrss;
#if defined(__linux__)
    peak_bytes *= 1024;
#endif

    fprintf(stderr, "Peak memory usage: %.2f MiB (%zu bytes)\n",
            (double)peak_bytes / (1024.0 * 1024.0), peak_bytes);
    fprintf(stderr, "Execution time: %.3f s\n", elapsed);
}

typedef struct {
    char *name, *key;
    Image image;
    ClutImage clut;
    int tpag, crop_x, crop_y, crop_w, crop_h;
    size_t order;
} Asset;
typedef struct {
    Asset *items; size_t count;
    const Options *options;
    pcre2_code **patterns;
} Assets;
typedef struct { int atlas, x, y; } Location;

static bool starts(const char *s, const char *prefix) { return !strncmp(s,prefix,strlen(prefix)); }
static const char *name_or(const char *s, const char *prefix, size_t index, char **owned) {
    if (s) { *owned=NULL; return s; }
    *owned=format_string("%s%zu",prefix,index);
    return *owned;
}
static char *sprite_key(const char *name) {
    const char *prefixes[] = {
        "spr_floweyx_tv","spr_tvinside","spr_lefteye_","spr_flipeye_","spr_floweyx_mouth",
        "spr_floweyx_dimple","spr_mouthball","spr_mouthbeam","spr_mouthflash","spr_floweyx_fleshmound",
        "spr_fleshmound","spr_nostrils","spr_dentata_","spr_halfdentata_","spr_floweyx_flame",
        "spr_floweynuke","spr_venus_placeholder","spr_bgpipe","spr_pipepart","spr_sidestalk",
        "spr_vines_flowey","spr_floweyarm","spr_tv_warning","spr_tv_exface","spr_tv_floweyface",
        "spr_tv_floweyx_","spr_noise"
    };
    for (size_t i=0;i<sizeof prefixes/sizeof *prefixes;++i)
        if (starts(name,prefixes[i])) return copy_string("spr/_omega_flowey");
    return format_string("spr/%s",name);
}
static int max_dimension(const char *name, int size) {
    const struct { const char *name; int size; } rules[] = {
        {"spr_sidestalk",16},{"spr_mouthball_",32},{"spr_fa_seq_b_",64},{"spr_floweyarm",64},
        {"spr_fa_stemunder_",64},{"spr_floweynuke_explosion",16},{"spr_floweyx_flame_move_",16},
        {"spr_venus_placeholder",32},{"spr_f_handgun_neo_",32},{"spr_vines_flowey",32},
        {"spr_floweyx_flamethrower",64},{"spr_floweynuke",32},{"spr_bgpipe",64},
        {"spr_tvinside_old",64},{"spr_floweyx_tv",128},{"spr_floweyx_fleshmound",64},
        {"spr_tv_floweyx_laugh",64},{"spr_floweyx_mouthedge",64}
    };
    if (!starts(name,"spr/")) return size;
    for (size_t i=0;i<sizeof rules/sizeof *rules;++i)
        if (starts(name+4,rules[i].name)) return rules[i].size;
    return size;
}
static Image subimage(const Image *src,int x,int y,int w,int h) {
    if (x<0 || y<0 || w<1 || h<1 || x>src->width-w || y>src->height-h)
        fail("invalid subimage rectangle");
    Image out=new_image(w,h);
    for (int j=0;j<h;++j)
        memcpy(out.pixels+(size_t)j*w,src->pixels+(size_t)(y+j)*src->width+x,(size_t)w*4);
    return out;
}
static void debug_png(const Options *o,const char *dir,const char *name,const Image *img) {
    if (!o->debug) return;
    if (!*name || !strcmp(name,".") || !strcmp(name,"..") || strchr(name,'/') || strchr(name,'\\'))
        fail("unsafe debug image name '%s'",name);
    char *directory=join_path(o->debug,dir);
    mkdirs(directory);
    char *filename=format_string("%s.png",name);
    char *path=join_path(directory,filename);
    Image rendered=new_image(img->width,img->height);
    for (size_t i=0;i<(size_t)img->width*img->height;++i)
        rendered.pixels[i]=img->pixels[i]>>24 ? img->pixels[i]:0;
    save_png(path,&rendered);
    free_image(&rendered);
    free(directory); free(filename); free(path);
}
static void debug_tpag(const Options *o,const char *dir,const char *name,
                       const Tpag *t,const Image *pages,size_t count) {
    if (!o->debug) return;
    Image out=new_image(t->bw>0?t->bw:1,t->bh>0?t->bh:1);
    if (t->page>=0 && (size_t)t->page<count && pages[t->page].pixels && t->tw && t->th && t->sw && t->sh) {
        const Image *src=&pages[t->page];
        for (int y=0;y<t->th && t->ty+y<out.height;++y) {
            int sy=t->sy+(int)(((int64_t)y*2+1)*t->sh/(2*t->th));
            if (sy>=src->height) continue;
            for (int x=0;x<t->tw && t->tx+x<out.width;++x) {
                int sx=t->sx+(int)(((int64_t)x*2+1)*t->sw/(2*t->tw));
                if (sx<src->width)
                    out.pixels[(size_t)(t->ty+y)*out.width+t->tx+x]=src->pixels[(size_t)sy*src->width+sx];
            }
        }
    }
    debug_png(o,dir,name,&out);
    free_image(&out);
}
static void prepare_asset(Asset *a,int atlas_size);
static void add_asset(Assets *assets,char *name,char *key,Image image,int tpag,size_t order) {
    Asset asset={.name=name,.key=key,.image=image,.tpag=tpag,
                 .crop_w=image.width,.crop_h=image.height,.order=order};
    prepare_asset(&asset,assets->options->atlas_size);
    bool force=false;
    for (size_t j=0;j<assets->options->pattern_count;++j) {
        pcre2_match_data *match=pcre2_match_data_create_from_pattern(assets->patterns[j],NULL);
        if (!match) fail("cannot allocate regex match data");
        int rc=pcre2_match(assets->patterns[j],(PCRE2_SPTR)name,strlen(name),
                          0,PCRE2_ANCHORED|PCRE2_ENDANCHORED,match,NULL);
        pcre2_match_data_free(match);
        if (rc>=0) force=true;
        else if (rc!=PCRE2_ERROR_NOMATCH) fail("regex matching failed (%d) for '%s'",rc,name);
    }
    asset.clut=make_clut(name,&asset.image,force);
    free_image(&asset.image);
    assets->items=resize_array(assets->items,assets->count+1,sizeof *assets->items);
    assets->items[assets->count++]=asset;
}
static int compare_asset_order(const void *a,const void *b) {
    size_t left=((const Asset *)a)->order,right=((const Asset *)b)->order;
    return (left>right)-(left<right);
}
static void prepare_asset(Asset *a,int atlas_size) {
    Image *img=&a->image;
    if (starts(a->name,"spr/")) {
        int minx=img->width,miny=img->height,maxx=-1,maxy=-1;
        for (int y=0;y<img->height;++y) for (int x=0;x<img->width;++x)
            if (img->pixels[(size_t)y*img->width+x]>>24) {
                if (x<minx) minx=x;
                if (y<miny) miny=y;
                if (x>maxx) maxx=x;
                if (y>maxy) maxy=y;
            }
        Image cropped;
        if (maxx<0) { cropped=new_image(1,1); minx=0; miny=0; }
        else cropped=subimage(img,minx,miny,maxx-minx+1,maxy-miny+1);
        free_image(img); *img=cropped;
        a->crop_x=minx; a->crop_y=miny; a->crop_w=img->width; a->crop_h=img->height;
    }
    int max=max_dimension(a->name,atlas_size);
    if (img->width>max || img->height>max) {
        double sx=(double)max/img->width,sy=(double)max/img->height,scale=sx<sy?sx:sy;
        int w=(int)(img->width*scale),h=(int)(img->height*scale);
        if (w<1) w=1;
        if (h<1) h=1;
        Image resized=new_image(w,h);
        for (int y=0;y<h;++y) for (int x=0;x<w;++x)
            resized.pixels[(size_t)y*w+x]=img->pixels[(size_t)((int64_t)y*img->height/h)*img->width+(int64_t)x*img->width/w];
        free_image(img); *img=resized;
    }
}
static int tile_tpag(const DataWin *dw,const Tile *t) {
    if (t->sprite) {
        const Sprite *s=&dw->sprites[t->definition];
        return s->count ? resolve_tpag(dw,s->textures[0]):-1;
    }
    return resolve_tpag(dw,dw->backgrounds[t->definition].texture);
}
static char *tile_name(const DataWin *dw,const Tile *t,bool key) {
    const char *n=t->sprite ? dw->sprites[t->definition].name:dw->backgrounds[t->definition].name;
    char *fallback=NULL;
    if (!n) { fallback=format_string("%s%d",t->sprite ? "spr":"bg",t->definition); n=fallback; }
    char *out=key ? format_string("tile/%s",n):
        format_string("tile/%s_%d_%d_%dx%d",n,t->x,t->y,t->width,t->height);
    free(fallback);
    return out;
}
static void collect_page_assets(Assets *a,const Options *o,const DataWin *dw,Image *pages,size_t page_index) {
    if (page_index>=dw->texture_count) return;
    if (!pages[page_index].pixels && dw->textures[page_index].size)
        pages[page_index]=decode_image(dw->textures[page_index],version_at_least(dw,2022,5,0,0));
    if (!pages[page_index].pixels) return;
    size_t order=0;
    for (size_t i=0;i<dw->sprite_count;++i) {
        const Sprite *sp=&dw->sprites[i];
        char *owned;
        const char *n=name_or(sp->name,"sprite_",i,&owned);
        for (size_t j=0;j<sp->count;++j,++order) {
            int index=resolve_tpag(dw,sp->textures[j]);
            if (index<0 || dw->tpags[index].page!=(int)page_index) continue;
            Image img=extract_tpag(&dw->tpags[index],pages,dw->texture_count);
            char *name=sp->count>1 ? format_string("%s_%zu",n,j):copy_string(n);
            debug_tpag(o,"sprites",name,&dw->tpags[index],pages,dw->texture_count);
            char *full=format_string("spr/%s",name);
            add_asset(a,full,sprite_key(n),img,index,order);
            free(name);
        }
        free(owned);
    }
    for (int which=0;which<2;++which) {
        const NamedTexture *items=which ? dw->fonts:dw->backgrounds;
        size_t count=which ? dw->font_count:dw->background_count;
        for (size_t i=0;i<count;++i,++order) {
            int index=resolve_tpag(dw,items[i].texture);
            if (index<0 || dw->tpags[index].page!=(int)page_index) continue;
            char *owned;
            const char *n=name_or(items[i].name,which?"font_":"bg_",i,&owned);
            Image img=extract_tpag(&dw->tpags[index],pages,dw->texture_count);
            debug_tpag(o,which?"fonts":"backgrounds",n,&dw->tpags[index],pages,dw->texture_count);
            char *name=format_string("%s/%s",which?"font":"bg",n);
            add_asset(a,name,copy_string(name),img,index,order);
            free(owned);
        }
    }
    for (size_t i=0;i<dw->tile_count;++i,++order) {
        const Tile *t=&dw->tiles[i];
        int index=tile_tpag(dw,t);
        if (index<0 || t->x<0 || t->y<0 || t->width<=0 || t->height<=0 || dw->tpags[index].page!=(int)page_index) continue;
        Image source=extract_tpag(&dw->tpags[index],pages,dw->texture_count);
        if (t->x>=source.width || t->y>=source.height) { free_image(&source); continue; }
        int w=t->width<source.width-t->x ? t->width:source.width-t->x;
        int h=t->height<source.height-t->y ? t->height:source.height-t->y;
        Image img=subimage(&source,t->x,t->y,w,h);
        free_image(&source);
        char *name=tile_name(dw,t,false);
        debug_png(o,"tiles",name+5,&img);
        add_asset(a,name,tile_name(dw,t,true),img,-1,order);
    }
}

static Assets collect(const Options *o,const DataWin *dw,Image *pages,pcre2_code **patterns) {
    Assets a={.options=o,.patterns=patterns};
    if (o->debug) {
        const char *dirs[]={"sprites","backgrounds","fonts","tiles","atlas_debug"};
        for (size_t i=0;i<5;++i) { char *path=join_path(o->debug,dirs[i]); mkdirs(path); free(path); }
    }
    bool *used=allocate(dw->texture_count,sizeof *used);
    for (size_t i=0;i<dw->sprite_count;++i) {
        const Sprite *sp=&dw->sprites[i];
        for (size_t j=0;j<sp->count;++j) {
            int index=resolve_tpag(dw,sp->textures[j]);
            if (index>=0 && dw->tpags[index].page>=0 && (size_t)dw->tpags[index].page<dw->texture_count)
                used[(size_t)dw->tpags[index].page]=true;
        }
    }
    for (int which=0;which<2;++which) {
        const NamedTexture *items=which ? dw->fonts:dw->backgrounds;
        size_t count=which ? dw->font_count:dw->background_count;
        for (size_t i=0;i<count;++i) {
            int index=resolve_tpag(dw,items[i].texture);
            if (index>=0 && dw->tpags[index].page>=0 && (size_t)dw->tpags[index].page<dw->texture_count)
                used[(size_t)dw->tpags[index].page]=true;
        }
    }
    for (size_t i=0;i<dw->tile_count;++i) {
        const Tile *t=&dw->tiles[i];
        int index=tile_tpag(dw,t);
        if (index>=0 && dw->tpags[index].page>=0 && (size_t)dw->tpags[index].page<dw->texture_count)
            used[(size_t)dw->tpags[index].page]=true;
    }
    for (size_t i=0;i<dw->texture_count;++i) {
        if (!used[i]) continue;
        collect_page_assets(&a,o,dw,pages,i);
        free_image(&pages[i]);
    }
    free(used);
    /* Palette merging and packing use source order to break ties, not page order. */
    if (a.count>1) qsort(a.items,a.count,sizeof *a.items,compare_asset_order);
    return a;
}
static void output(const Options *o,const char *name,Buffer *buffer) {
    char *path=join_path(o->output,name);
    write_file(path,buffer->data,buffer->size);
    if (bspp_verbose) printf("Wrote %zu bytes to %s\n",buffer->size,path);
    free(path); free(buffer->data); *buffer=(Buffer){0};
}
static int group_row(const Groups *groups,int id) {
    int row4=0,row8=0;
    for (size_t i=0;i<groups->count;++i) {
        ClutGroup *g=&groups->items[i];
        int row=g->bpp==4 ? row4++:row8++;
        if (g->id==id) return row;
    }
    fail("missing CLUT group %d",id);
}
static uint32_t ps2_rgba_from_argb(uint32_t argb) {
    uint32_t a = (argb >> 24) & 255u;
    uint32_t r = (argb >> 16) & 255u;
    uint32_t g = (argb >> 8) & 255u;
    uint32_t b = argb & 255u;
    uint32_t ps2_alpha = (a + 1u) >> 1;
    return (ps2_alpha << 24) | (b << 16) | (g << 8) | r;
}

static void clut_files(const Options *o,const Groups *groups) {
    Buffer four={0},eight={0};
    for (size_t i=0;i<groups->count;++i) {
        const ClutGroup *g=&groups->items[i];
        int slots=g->bpp==4 ? 16:256;
        Buffer *out=g->bpp==4 ? &four:&eight;

        uint32_t palette[256];
        for (int j=0;j<slots;++j) palette[j]=g->palette[j];
        for (int j=0;j<slots;++j) palette[j]=ps2_rgba_from_argb(palette[j]);

        if (slots == 256) {
            for (int j=0;j<slots;++j) {
                if ((j & 0x18) == 8) {
                    uint32_t tmp = palette[j];
                    palette[j] = palette[j + 8];
                    palette[j + 8] = tmp;
                }
            }
        }

        for (int j=0;j<slots;++j) {
            uint32_t c=palette[j];
            put8(out, c & 255u);
            put8(out, (c >> 8) & 255u);
            put8(out, (c >> 16) & 255u);
            put8(out, (c >> 24) & 255u);
        }
    }
    output(o,"CLUT4.BIN",&four); output(o,"CLUT8.BIN",&eight);
}
static void file_write_u8(FILE *file, unsigned value) {
    uint8_t byte=(uint8_t)value;
    if (fwrite(&byte,1,1,file)!=1) fail("cannot write output byte");
}
static void file_write_u16(FILE *file, uint64_t value, bool big) {
    uint8_t bytes[2] = { (uint8_t)(value >> (big ? 8 : 0)), (uint8_t)(value >> (big ? 0 : 8)) };
    if (fwrite(bytes,1,2,file)!=2) fail("cannot write output u16");
}
static void file_write_u32(FILE *file, uint64_t value, bool big) {
    for (unsigned i=0;i<4;++i)
        file_write_u8(file,(unsigned)(value >> ((big ? 3-i : i) * 8)) & 255);
}
static void file_write_bytes(FILE *file, const void *data, size_t size) {
    if (size && fwrite(data,1,size,file)!=size) fail("cannot write output bytes");
}
static size_t rle_size(const uint8_t *data,size_t size) {
    size_t out=0;
    for (size_t i=0;i<size;) {
        size_t n=1;
        while (n<255 && i+n<size && data[i+n]==data[i]) ++n;
        out += 2;
        i += n;
    }
    return out;
}
static size_t write_rle(FILE *out,const uint8_t *data,size_t size) {
    size_t written=0;
    for (size_t i=0;i<size;) {
        size_t n=1;
        while (n<255 && i+n<size && data[i+n]==data[i]) ++n;
        uint8_t count=(uint8_t)n;
        if (fwrite(&count,1,1,out)!=1) fail("cannot write output run-length byte");
        if (fwrite(data+i,1,1,out)!=1) fail("cannot write output run-length value");
        written += 2;
        i += n;
    }
    return written;
}
static void debug_atlas(const Options *o,const Atlas *a,const ClutImage *images) {
    if (!o->debug) return;
    const uint32_t colors[]={0xffff0000,0xff00ff00,0xff0000ff,0xffffff00,0xff00ffff,0xffff00ff,0xffffc800};
    Image out=new_image(a->width,a->height);
    for (size_t i=0;i<a->count;++i) {
        Placement p=a->entries[i];
        const ClutImage *img=&images[p.image];
        for (int y=0;y<img->height;++y) for (int x=0;x<img->width;++x)
            out.pixels[(size_t)(p.y+y)*out.width+p.x+x]=img->palette[img->indices[(size_t)y*img->width+x]];
    }
    for (size_t i=0;i<a->count;++i) {
        Placement p=a->entries[i]; const ClutImage *img=&images[p.image]; uint32_t c=colors[i%7];
        for (int x=0;x<img->width;++x) {
            out.pixels[(size_t)p.y*out.width+p.x+x]=c;
            out.pixels[(size_t)(p.y+img->height-1)*out.width+p.x+x]=c;
        }
        for (int y=0;y<img->height;++y) {
            out.pixels[(size_t)(p.y+y)*out.width+p.x]=c;
            out.pixels[(size_t)(p.y+y)*out.width+p.x+img->width-1]=c;
        }
    }
    char *name=format_string("atlas_%d_%dbpp",a->id,a->bpp);
    debug_png(o,"atlas_debug",name,&out); free(name); free_image(&out);
}
static void free_decoded_pages(Image **pages, size_t count) {
    if (!pages || !*pages) return;
    for (size_t i=0;i<count;++i) free_image(&(*pages)[i]);
}
static void metadata_entry_file(FILE *out,const Asset *a,const ClutImage *image,
                               Location location,const Groups *groups) {
    file_write_u16(out,location.atlas<0 ? 65535:(unsigned)location.atlas,false);
    file_write_u16(out,location.x,false); file_write_u16(out,location.y,false);
    file_write_u16(out,image ? image->width:0,false); file_write_u16(out,image ? image->height:0,false);
    file_write_u16(out,a ? a->crop_x:0,false); file_write_u16(out,a ? a->crop_y:0,false);
    file_write_u16(out,a ? a->crop_w:0,false); file_write_u16(out,a ? a->crop_h:0,false);
    file_write_u16(out,image ? group_row(groups,image->group):0,false);
}
static void ps2(const Options *o,const DataWin *dw,Image *pages) {
    pcre2_code **patterns=allocate(o->pattern_count,sizeof *patterns);
    for (size_t i=0;i<o->pattern_count;++i) {
        int err; PCRE2_SIZE off;
        patterns[i]=pcre2_compile((PCRE2_SPTR)o->patterns[i],PCRE2_ZERO_TERMINATED,PCRE2_UTF,&err,&off,NULL);
        if (!patterns[i]) {
            PCRE2_UCHAR message[256];
            pcre2_get_error_message(err,message,sizeof message);
            fail("invalid --force-4bpp regex at byte %zu: %s",(size_t)off,(char *)message);
        }
    }
    Assets assets=collect(o,dw,pages,patterns);
    free_decoded_pages(&pages,dw->texture_count);
    if (bspp_verbose) printf("Processing %zu images\n",assets.count);
    ClutImage *images=allocate(assets.count,sizeof *images);
    const char **keys=allocate(assets.count,sizeof *keys);
    Location *locations=allocate(assets.count,sizeof *locations);
    for (size_t i=0;i<assets.count;++i) {
        images[i]=assets.items[i].clut;
        keys[i]=assets.items[i].key;
        locations[i].atlas=-1;
    }
    for (size_t i=0;i<o->pattern_count;++i) pcre2_code_free(patterns[i]);
    free(patterns);
    Groups groups=merge_cluts(images,assets.count);
    Atlases atlases=pack_atlases(images,keys,assets.count,o->atlas_size);
    clut_files(o,&groups);
    char *textures_path=join_path(o->output,"TEXTURES.BIN");
    FILE *textures_file=fopen(textures_path,"wb");
    if (!textures_file) fail("cannot create '%s': %s",textures_path,strerror(errno));
    char *metadata_path=join_path(o->output,"ATLAS.BIN");
    FILE *metadata_file=fopen(metadata_path,"wb");
    if (!metadata_file) fail("cannot create '%s': %s",metadata_path,strerror(errno));
    file_write_u8(metadata_file,0); file_write_u16(metadata_file,dw->tpag_count,false);
    file_write_u16(metadata_file,dw->tile_count,false); file_write_u16(metadata_file,atlases.count,false);
    size_t textures_size=0;
    for (size_t i=0;i<atlases.count;++i) {
        const Atlas *a=&atlases.items[i];
        size_t area=(size_t)a->width*a->height;
        uint8_t *canvas=allocate(area,1);
        for (size_t j=0;j<a->count;++j) {
            Placement p=a->entries[j]; const ClutImage *img=&images[p.image];
            locations[p.image]=(Location){a->id,p.x,p.y};
            for (int y=0;y<img->height;++y)
                memcpy(canvas+(size_t)(p.y+y)*a->width+p.x,img->indices+(size_t)y*img->width,(size_t)img->width);
        }
        Buffer pixels={0};
        if (a->bpp==4) {
            for (size_t j=0;j<area;j+=2) put8(&pixels,canvas[j]|(j+1<area ? canvas[j+1]<<4:0));
        } else append(&pixels,canvas,area);
        free(canvas);
        size_t rle_size_value = rle_size(pixels.data,pixels.size);
        bool use = rle_size_value < pixels.size;
        size_t payload_size = use ? rle_size_value : pixels.size;
        file_write_u32(metadata_file,textures_size,false); file_write_u16(metadata_file,a->width,false); file_write_u16(metadata_file,a->height,false);
        file_write_u8(metadata_file,a->bpp); file_write_u32(metadata_file,payload_size,false); file_write_u8(metadata_file,use ? 1:0);
        if (use) {
            write_rle(textures_file,pixels.data,pixels.size);
        } else {
            file_write_bytes(textures_file,pixels.data,pixels.size);
        }
        textures_size += payload_size;
        free(pixels.data);
        debug_atlas(o,a,images);
    }
    for (size_t i=0;i<dw->tpag_count;++i) {
        size_t found=SIZE_MAX;
        for (size_t j=0;j<assets.count;++j) if (assets.items[j].tpag==(int)i) found=j;
        if (found==SIZE_MAX) {
            file_write_u16(metadata_file,65535,false); file_write_u16(metadata_file,0,false); file_write_u16(metadata_file,0,false);
            file_write_u16(metadata_file,0,false); file_write_u16(metadata_file,0,false); file_write_u16(metadata_file,0,false);
            file_write_u16(metadata_file,0,false); file_write_u16(metadata_file,0,false); file_write_u16(metadata_file,0,false);
            file_write_u16(metadata_file,0,false); file_write_u16(metadata_file,0,false); file_write_u16(metadata_file,0,false);
        } else {
            metadata_entry_file(metadata_file,&assets.items[found],&images[found],locations[found],&groups);
        }
    }
    for (size_t i=0;i<dw->tile_count;++i) {
        const Tile *t=&dw->tiles[i]; char *name=tile_name(dw,t,false);
        size_t found=SIZE_MAX;
        for (size_t j=0;j<assets.count;++j) if (!strcmp(name,assets.items[j].name)) found=j;
        free(name);
        file_write_u16(metadata_file,t->definition,false); file_write_u16(metadata_file,t->x,false); file_write_u16(metadata_file,t->y,false);
        file_write_u16(metadata_file,t->width,false); file_write_u16(metadata_file,t->height,false);
        if (found==SIZE_MAX) {
            file_write_u16(metadata_file,65535,false); file_write_u16(metadata_file,0,false); file_write_u16(metadata_file,0,false);
            file_write_u16(metadata_file,0,false); file_write_u16(metadata_file,0,false); file_write_u16(metadata_file,0,false);
            file_write_u16(metadata_file,0,false); file_write_u16(metadata_file,0,false); file_write_u16(metadata_file,0,false);
            file_write_u16(metadata_file,0,false); file_write_u16(metadata_file,0,false); file_write_u16(metadata_file,0,false);
        } else {
            metadata_entry_file(metadata_file,&assets.items[found],&images[found],locations[found],&groups);
        }
    }
    long metadata_size = ftell(metadata_file);
    if (metadata_size < 0) fail("cannot tell '%s': %s",metadata_path,strerror(errno));
    if (fclose(textures_file)) fail("cannot close '%s': %s",textures_path,strerror(errno));
    if (fclose(metadata_file)) fail("cannot close '%s': %s",metadata_path,strerror(errno));
    if (bspp_verbose) {
        printf("Wrote %zu bytes to %s\n",textures_size,textures_path);
        printf("Wrote %zu bytes to %s\n",(size_t)metadata_size,metadata_path);
    }
    free(textures_path); free(metadata_path);
    for (size_t i=0;i<assets.count;++i) {
        free(assets.items[i].name); free(assets.items[i].key); free_image(&assets.items[i].image); free_clut(&images[i]);
    }
    free(assets.items); free(images); free(keys); free(locations); free(groups.items); free_atlases(&atlases);
    char *directory=copy_string(o->input),*slash=strrchr(directory,'/');
    if (slash) { if (slash==directory) slash[1]='\0'; else *slash='\0'; }
    else { free(directory); directory=copy_string("."); }
    char *sounds_path=join_path(o->output,"SOUNDS.BIN");
    FILE *sounds=fopen(sounds_path,"wb");
    if (!sounds) fail("cannot create '%s': %s",sounds_path,strerror(errno));
    Buffer bank={0};
    process_audio(dw,directory,&bank,sounds);
    long sounds_size=ftell(sounds);
    if (sounds_size<0) fail("cannot tell '%s': %s",sounds_path,strerror(errno));
    if (fclose(sounds)) fail("cannot close '%s': %s",sounds_path,strerror(errno));
    output(o,"SOUNDBNK.BIN",&bank);
    if (bspp_verbose) printf("Wrote %zu bytes to %s\n",(size_t)sounds_size,sounds_path);
    free(sounds_path); free(directory);
}
static void ps3(const Options *o,const DataWin *dw,Image *pages) {
    ClutImage *images=allocate(dw->tpag_count,sizeof *images);
    int *mapping=allocate(dw->tpag_count,sizeof *mapping);
    size_t count=0;
    for (size_t i=0;i<dw->tpag_count;++i) {
        const Tpag *t=&dw->tpags[i]; mapping[i]=-1;
        if (t->page<0 || (size_t)t->page>=dw->texture_count || !dw->textures[t->page].size || !t->tw || !t->th) continue;
        mapping[i]=(int)count++;
    }
    for (size_t page=0;page<dw->texture_count;++page) {
        if (!dw->textures[page].size) continue;
        pages[page]=decode_image(dw->textures[page],version_at_least(dw,2022,5,0,0));
        for (size_t i=0;i<dw->tpag_count;++i) {
            if (mapping[i]<0 || dw->tpags[i].page!=(int)page) continue;
            Image img=extract_tpag(&dw->tpags[i],pages,dw->texture_count);
            char *name=format_string("tpag_%zu",i);
            images[mapping[i]]=make_clut(name,&img,false);
            images[mapping[i]].bpp=8;
            free(name); free_image(&img);
        }
        free(pages[page].pixels);
        pages[page].pixels=NULL;
    }
    Groups groups=merge_cluts(images,count);
    Buffer out={0};
    put8(&out,0); put16(&out,groups.count,true); put16(&out,dw->texture_count,true); put16(&out,dw->tpag_count,true);
    for (size_t i=0;i<groups.count;++i) for (int j=0;j<256;++j) {
        uint32_t c=groups.items[i].palette[j];
        put8(&out,(c>>16)&255); put8(&out,(c>>8)&255); put8(&out,c&255); put8(&out,c>>24);
    }
    for (size_t i=0;i<dw->tpag_count;++i) {
        int index=mapping[i];
        put16(&out,index<0 ? 65535:group_row(&groups,images[index].group),true);
    }
    size_t cursor=0;
    for (size_t i=0;i<dw->texture_count;++i) {
        size_t size=(size_t)pages[i].width*pages[i].height;
        put16(&out,pages[i].width,true); put16(&out,pages[i].height,true);
        put32(&out,cursor,true); put32(&out,size,true);
        if (size>UINT32_MAX-cursor) fail("PS3 page data exceeds 32-bit offsets");
        cursor+=size;
    }
    char *path=join_path(o->output,"textures.bin");
    FILE *file=fopen(path,"wb");
    if (!file) fail("cannot create '%s': %s",path,strerror(errno));
    file_write_bytes(file,out.data,out.size);
    size_t output_size=out.size+cursor;
    free(out.data);
    for (size_t p=0;p<dw->texture_count;++p) {
        const Image *page=&pages[p];
        size_t size=(size_t)page->width*page->height;
        uint8_t *canvas=allocate(size,1);
        for (size_t i=0;i<dw->tpag_count;++i) {
            if (mapping[i]<0 || dw->tpags[i].page!=(int)p) continue;
            const Tpag *t=&dw->tpags[i]; const ClutImage *img=&images[mapping[i]];
            for (int y=0;y<t->th;++y) for (int x=0;x<t->tw;++x)
                if (t->ty+y<img->height && t->sy+y<page->height && t->tx+x<img->width && t->sx+x<page->width)
                    canvas[(size_t)(t->sy+y)*page->width+t->sx+x]=img->indices[(size_t)(t->ty+y)*img->width+t->tx+x];
        }
        file_write_bytes(file,canvas,size);
        free(canvas);
    }
    if (fclose(file)) fail("cannot close '%s': %s",path,strerror(errno));
    if (bspp_verbose) printf("Wrote %zu bytes to %s\n",output_size,path);
    free(path);
    for (size_t i=0;i<count;++i) free_clut(&images[i]);
    free(images); free(mapping); free(groups.items);
}
void process(const Options *o) {
    struct timespec start = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &start)) start = (struct timespec){0};
    bspp_verbose = o->verbose;
    DataWin dw=parse_datawin(read_file(o->input),false,o->ps3);
    mkdirs(o->output);
    Image *pages=allocate(dw.texture_count,sizeof *pages);
    if (o->ps3) ps3(o,&dw,pages);
    else ps2(o,&dw,pages);
    for (size_t i=0;i<dw.texture_count;++i) free_image(&pages[i]);
    free(pages); free_datawin(&dw);
    if (bspp_verbose) puts("Done!");
    print_runtime_summary(&start);
}
