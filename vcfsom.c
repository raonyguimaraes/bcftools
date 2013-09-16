#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <math.h>
#include <time.h>
#include <htslib/bgzf.h>
#include <htslib/vcf.h>
#include <htslib/synced_bcf_reader.h>
#include <htslib/vcfutils.h>
#include <inttypes.h>
#include "bcftools.h"

#define NFIXED 5
#define MASK_GOOD 2
#define IS_GOOD(mask) ((mask)&MASK_GOOD) 

typedef struct
{
    int nsom;           // number of maps to average for greater robustness
    int nbin, kdim;     // number of bins and dimension
    int nt, *t;         // total number of learning cycles and the current cycle
    double *w, *c;      // weights and counts (sum of learning influence)
    double learn, th;   // SOM parameters
}
som_t;

typedef struct
{
    unsigned int ngood, nall, nmissing;
    double good_min, good_max;      // extremes of sites in the good set
    double all_min, all_max;        // overall extremes
    double scale_min, scale_max;    // 0.5 and 0.95 percentile
}
dist_t;

#define FLT_LE  2       // less or equal
#define FLT_LT  1       // less than
#define FLT_EQ  0       // equal
#define FLT_BT -1       // bigger than
#define FLT_BE -2       // bigger or equal

typedef struct
{
    int type;           // one of the FLT_* keys above
    double value;       // threshold to use for filtering
    char *desc;         // for annotating the FILTER column
}
filter_t;

typedef struct
{
    int *nfilt, nann, ntot; // nfilt: number of filters for each annotation; nann: number of annotations; ntot: number of filters in total
    filter_t **filt;        // filters for each annotation
    char **flt_mask_desc;
}
filters_t;

// indel context functions
typedef struct indel_ctx_t indel_ctx_t;
indel_ctx_t *indel_ctx_init(char *fa_ref_fname);
void indel_ctx_destroy(indel_ctx_t *ctx);
int indel_ctx_type(indel_ctx_t *ctx, char *chr, int pos, char *ref, char *alt, int *nrep, int *nlen);

typedef struct
{   
    char **names;               // annotation names (0..nann-1), list of annotations actually used
    char **colnames;            // all columns' names
    int *col2names;             // mapping from column number to i-th annot. The index of unused annots is set to -1. Includes first NFIXED columns.
    int *ann2cols;              // mapping from i-th annot to j-th column of annot.tab.gz. First annotation has index NFIXED.
    int nann, nann_som;         // number of used annotations (total, included fixed filters) and for SOM
    int ncols;                  // number of columns (total, including first NFIXED columns)
    dist_t *dists;              // annots distributions (all annots, including those not requested)
    int ngood, nall;            // number of good sites (min across all annots) and all sites
    int scale;                  // should the annotations be rescaled according to lo_,hi_pctl?
    double lo_pctl, hi_pctl;    // the percentage of sites to zoom out at both ends

    // annots_reader_* data
    htsFile *file;              // reader
    kstring_t str;              // temporary string for annots_reader_*
    int *ignore;                // columns to ignore (ncols, ro)
    int pos, mask;              // filled by annots_reader_next
    char *chr, *ref, *alt;      // filled by annots_reader_next
    double *vals;               // filled by annots_reader_next (ncols), scaled if scale set
    double *raw_vals;           // same as vals, unscaled
    int *missing;               // columns with missing values, filled by annots_reader_next (ncols)
    int nset, nset_mask;        // number of filled coordinates and their mask, filled by annots_reader_next

    som_t som, *good_som;
    filters_t filt_learn;
    double snp_th, indel_th, nt_learn_frac;
    char *snp_sites_fname, *indel_sites_fname;
    int filt_type;              // calculate ts/tv or indel consistency

    indel_ctx_t *indel_ctx;     // indel context

    int good_mask;
    int rand_seed;
    char *annot_str, *learning_filters;
	char **argv, *fname, *out_prefix, *region, *ref_fname;
    char *sort_args;
	int argc, unset_unknowns, output_type;
}
args_t;

static void usage(void);
FILE *open_file(char **fname, const char *mode, const char *fmt, ...);
void mkdir_p(const char *fmt, ...);

/**
 *  ks_getline() - Read next line from $fp, appending it to $str.  The newline
 *  is stripped and \0 appended. Returns the number of characters read
 *  excluding the null byte.
 */
size_t ks_getline(FILE *fp, kstring_t *str)
{
    size_t nread=0;
    int c;
    while ((c=getc(fp))!= EOF && c!='\n')
    {
        nread++;
        if ( str->l+nread > str->m )
        {
            str->m += 1024;
            str->s = (char*) realloc(str->s, sizeof(char)*str->m);
        }
        str->s[str->l+nread-1] = c;
    }
    if ( str->l >= str->m )
    {
        str->m += 1024;
        str->s = (char*) realloc(str->s, sizeof(char)*str->m);
    }
    str->l += nread;
    str->s[ str->l ] = 0;
    return nread;
}
char *msprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap) + 2;
    va_end(ap);

    char *str = (char*)malloc(n);
    va_start(ap, fmt);
    vsnprintf(str, n, fmt, ap);
    va_end(ap);

    return str;
}
static char *_strndup(const char *ptr, int len)
{
    char *tmp = (char*) malloc(sizeof(char)*(len+1));
    int i;
    for (i=0; i<len; i++) 
    {
        if ( !ptr[i] ) break;
        tmp[i] = ptr[i];
    }
    tmp[i] = 0;
    return tmp;
}
char **read_list(char *fname, int *_n)
{
    int n = 0;
    char **list = NULL;

    FILE *fp = fopen(fname,"r");
    if ( !fp ) error("%s: %s\n", fname, strerror(errno));

    kstring_t str = {0,0,0};
    while ( ks_getline(fp, &str) )
    {
        list = (char**) realloc(list, sizeof(char*)*(++n));
        list[n-1] = strdup(str.s);
        str.l = 0;
    }
    fclose(fp);
    if ( str.m ) free(str.s);
    *_n = n;
    return list;
}
char **split_list(const char *str, int delim, int *n)
{
    *n = 0;
    char **out = NULL;
    const char *t = str, *p = t;
    while (*t)
    {
        if ( *t == delim ) 
        { 
            (*n)++;
            out = (char**) realloc(out, sizeof(char*)*(*n));
            out[(*n)-1] = _strndup(p,t-p);
            p = t+1;
        }
        t++;
    }
    (*n)++;
    out = (char**) realloc(out, sizeof(char*)*(*n));
    out[(*n)-1] = _strndup(p,t-p);
    return out;
}
void destroy_list(char **list, int n)
{
    int i;
    for (i=0; i<n; i++)
        free(list[i]);
    free(list);
}
void py_plot(char *script)
{
    mkdir_p(script);
    int len = strlen(script);
    char *cmd = !strcmp(".py",script+len-3) ? msprintf("python %s", script) : msprintf("python %s.py", script);
    int ret = system(cmd);
    if ( ret ) fprintf(stderr, "The command returned non-zero status %d: %s\n", ret, cmd);
    free(cmd);
}

/*
 *  char *t, *p = str;
 *  t = column_next(p, '\t'); 
 *  if ( strlen("<something>")==t-p && !strncmp(p,"<something>",t-p) ) printf("found!\n");
 *
 *  char *t;
 *  t = column_next(str, '\t'); if ( !*t ) error("expected field\n", str);
 *  t = column_next(t+1, '\t'); if ( !*t ) error("expected field\n", str);
 */
inline char *column_next(char *start, char delim)
{
    char *end = start;
    while (*end && *end!=delim) end++;
    return end;
}

inline double scale_value(dist_t *dist, double val)
{
    if ( val < dist->scale_min ) val = 0;
    else if ( val > dist->scale_max ) val = 1;
    else val = (val - dist->scale_min) / (dist->scale_max - dist->scale_min);
    assert( val>=0 && val<=1 );
    return val;
}
static int parse_mask(char *str)
{
    int i = 0, mask = 0;
    while ( str[i] )
    {
        if ( str[i]=='1' ) { mask |= 1<<i; }
        i++;
    }
    return mask;
}
static inline int str_mask_set(char *str, int mask)
{
    int i = 0;
    while ( str[i]=='0' || str[i]=='1' )
    {
        if ( str[i]=='1' && (mask & i<<1) ) return 1;
        i++;
    }
    return 0;
}
/**
 *  annots_reader_next() - reads next line from annots.tab.gz and sets the appropriate fields
 *
 *    mask:         2 if a good site, otherwise 1. Controlled by args->good_mask.
 *    type:         one of VCF_SNP, VCF_INDEL, VCF_OTHER
 *    nset:         number of non-missing values read into the $vals array
 *    nset_mask:    which values are non-missing. Note that this limits the number of maximum annotations
 *    vals:         the parsed values the order of which corresponds to $names (scaled to <0,1> if $scaled is set)
 *    raw_vals:     same as while $vals, but unscaled
 *    missing:      array of flags indicating whether the value is set or not (the same order as above)
 *
 *   In addition also these self-explanatory fields are set: chr, pos, ref, alt
 *
 *   Returns 1 on successful read or 0 if no further record could be read.
 */
int annots_reader_next(args_t *args)
{
    args->str.l = 0;
    if ( hts_getline(args->file,'\n',&args->str)<=0 ) return 0;

    char *t, *line = args->str.s;

    // CHR
    t = column_next(line, '\t'); 
    if ( !*t ) error("Could not parse CHR: [%s]\n", line);
    if ( !args->ignore[0] )
    {
        *t = 0;
        args->chr = line;
    }

    // POS
    t++;
    if ( !args->ignore[1] )
        args->pos = strtol(t, NULL, 10);
    t = column_next(t, '\t');  
    if ( !*t ) error("Could not parse POS: [%s]\n", line);

    // MASK
    if ( !args->ignore[2] )
        args->mask = args->good_mask ? 1 + str_mask_set(t+1, args->good_mask) : 1;
    t = column_next(t+1, '\t');  
    if ( !*t ) error("Could not parse MASK: [%s]\n", line);

    // REF
    args->ref = t+1;
    t = column_next(t+1, '\t');  
    if ( !*t ) error("Could not parse REF: [%s]\n", line);
    *t = 0;

    // ALT
    args->alt = t+1;
    t = column_next(t+1, '\t');  
    if ( !*t ) error("Could not parse ALT: [%s]\n", line);
    *t = 0;
    t++;

    args->nset = 0;
    args->nset_mask = 0;

    int icol, iann = -1;
    for (icol=NFIXED; icol<args->ncols; icol++)
    {
        if ( !*t ) error("Could not parse %d-th data field: is the line truncated?\nThe line was: [%s]\n",icol,line);

        if ( args->ignore[icol] )
        {
            // this column is to be ignored
            t = column_next(t+1,'\t');
            continue;
        }
        iann = args->col2names[icol];

        t++;
        if ( t[0]=='.' && (t[1]=='\t' || t[1]=='\n' || !t[1]) )
        {
            args->missing[iann] = 1;
            t++;
            continue;
        }
        if ( (sscanf(t,"%le", &args->vals[iann])!=1) ) 
            error("Could not parse %d-th data field: [%s]\nThe line was: [%s]\n", icol,t,line);

        if ( args->vals[iann] == HUGE_VAL || args->vals[iann] == -HUGE_VAL || isnan(args->vals[iann]) )
        {
            args->missing[iann] = 1;
            t = column_next(t,'\t');
            continue;
        }

        args->raw_vals[iann] = args->vals[iann];

        if ( args->scale && args->dists )
            args->vals[iann] = scale_value(&args->dists[icol], args->vals[iann]);

        args->nset++;
        args->nset_mask |= 1 << iann; 

        args->missing[iann] = 0;
        t = column_next(t,'\t');
    }
    return 1;
}
void annots_reader_reset(args_t *args)
{
    if ( args->file ) hts_close(args->file);
    if ( !args->fname ) error("annots_reader_reset: no fname\n");
    args->file = hts_open(args->fname, "r", NULL);
    hts_getline(args->file,'\n',&args->str);  // eat the header
}

static void init_filters(args_t *args, filters_t *filts, char *str, int scale);
static void destroy_filters(filters_t *filt);
static uint64_t failed_filters(filters_t *filt, double *vec);

static void create_dists(args_t *args)
{
    fprintf(stderr,"Sorting annotations and creating distribution stats: %s.n\n", args->out_prefix);

    // Create stats for all annotations in args->file. Sort each annotation using unix sort
    //  and determine percentiles.

    // Hackety hack: annots_reader_next will subset the annotations and return them in
    //  requested order. This is the only place where we want to read and return all the values
    //  as they are. Change the scale, ignore, and col2names arrays temporarily.
    //
    int *ignore_ori    = args->ignore;
    int *col2names_ori = args->col2names;
    args->ignore       = (int*) calloc(args->ncols, sizeof(int));
    args->col2names    = (int*) calloc(args->ncols, sizeof(int));
    args->missing      = (int*) calloc(args->ncols, sizeof(int));

    int nann_ori = args->nann; 
    args->nann   = args->ncols - NFIXED;

    // Open temporary files to store the annotations
    int i;
    dist_t *dists = (dist_t*) calloc(args->nann, sizeof(dist_t));
    FILE **fps = (FILE**) malloc(args->nann*sizeof(FILE*));
    for (i=0; i<args->nann; i++)
    {
        fps[i] = open_file(NULL,"w","%s.%s", args->out_prefix, args->colnames[i+NFIXED]);
        if ( !fps[i] ) error("%s: %s\n", args->str.s, strerror(errno));
        args->col2names[i + NFIXED] = i;
    }

    // Write each annotation in a separate file and collect min/max for all/good/hard-filtered sites
    annots_reader_reset(args);
    while ( annots_reader_next(args) )
    {
        for (i=0; i<args->nann; i++)
        {
            if ( args->missing[i] )
            {
                dists[i].nmissing++;
                continue;
            }
            if ( IS_GOOD(args->mask) )
            {
                if ( !dists[i].ngood ) dists[i].good_min = dists[i].good_max = args->raw_vals[i];
                if ( args->raw_vals[i] < dists[i].good_min ) dists[i].good_min = args->raw_vals[i];
                if ( args->raw_vals[i] > dists[i].good_max ) dists[i].good_max = args->raw_vals[i];
                dists[i].ngood++;
            }
            if ( !dists[i].nall ) dists[i].all_min = dists[i].all_max = args->raw_vals[i];
            if ( args->raw_vals[i] < dists[i].all_min ) dists[i].all_min = args->raw_vals[i];
            if ( args->raw_vals[i] > dists[i].all_max ) dists[i].all_max = args->raw_vals[i];
            dists[i].nall++;
            fprintf(fps[i], "%le\t%d\n", args->raw_vals[i], IS_GOOD(args->mask)?1:0);
        }
    }
    // Change the arrays back and clean
    free(args->ignore);
    free(args->col2names);
    args->ignore    = ignore_ori;
    args->col2names = col2names_ori;
    for (i=0; i<args->nann; i++)
        if ( fclose(fps[i]) ) error("An error occurred while processing %s.%s\n", args->out_prefix, args->colnames[i+NFIXED]);
    free(fps);

    // Find the 0.1st and 0.99th percentile to crop off outliers
    FILE *fp;
    kstring_t tmp = {0,0,0};
    for (i=0; i<args->nann; i++)
    {
        tmp.l = 0;
        ksprintf(&tmp, "cat %s.%s | sort -k1,1g ", args->out_prefix, args->colnames[i+NFIXED]);
        if ( args->sort_args )  kputs(args->sort_args, &tmp);
        fp = popen(tmp.s,"r");
        if ( !fp ) error("%s: %s\n", tmp.s, strerror(errno));
        fprintf(stderr,"sorting: %s\n", tmp.s);

        int count = 0, is_good;
        double val;
        dists[i].scale_min = dists[i].scale_max = HUGE_VAL;
        while ( fscanf(fp,"%le\t%d",&val,&is_good)==2 )
        {
            count++;
            if ( dists[i].scale_min==HUGE_VAL || (double)100.*count/dists[i].nall < args->lo_pctl ) dists[i].scale_min = val;
            if ( dists[i].scale_max==HUGE_VAL && (double)100.*count/dists[i].nall > args->hi_pctl ) dists[i].scale_max = val;
        }
        if ( dists[i].scale_max==HUGE_VAL ) dists[i].scale_max = val;
        if ( pclose(fp) ) error("An error occurred while processing %s.%s\n", args->out_prefix, args->colnames[i+NFIXED]);

        tmp.l = 0;
        ksprintf(&tmp, "%s.%s", args->out_prefix, args->colnames[i+NFIXED]);
        unlink(tmp.s);
    }
    free(tmp.s);

    fp = open_file(NULL,"w","%s.n", args->out_prefix);
    fprintf(fp, "# [1]nAll\t[2]nGood\t[3]nMissing\t[4]minGood\t[5]maxGood\t[6]minAll\t[7]maxAll\t[8]%f percentile\t[9]%f percentile\t[10]Annotation\n", args->lo_pctl, args->hi_pctl);
    for (i=0; i<args->nann; i++)
    {
        fprintf(fp, "%u\t%u\t%u\t%le\t%le\t%le\t%le\t%le\t%le\t%s\n", dists[i].nall, dists[i].ngood, dists[i].nmissing, 
            dists[i].good_min, dists[i].good_max, dists[i].all_min, dists[i].all_max, 
            dists[i].scale_min, dists[i].scale_max, args->colnames[i+NFIXED]);
    }
    fclose(fp);
    free(dists);
    args->nann = nann_ori;
}

static void init_dists(args_t *args)
{
    FILE *fp = open_file(NULL,"r","%s.n", args->out_prefix);
    if ( !fp ) fp = open_file(NULL,"r","%s.n", args->fname);
    else fprintf(stderr,"Re-using %s.n...\n", args->out_prefix);
    if ( !fp ) 
    {
        create_dists(args);
        fp = open_file(NULL,"r","%s.n", args->out_prefix);
    }
    else
        fprintf(stderr,"Re-using %s.n...\n", args->fname);
    if ( !fp ) error("Could not read %s.n nor %s.n\n", args->out_prefix,args->fname);

    args->dists = (dist_t*) calloc(args->ncols, sizeof(dist_t));
    args->str.l = 0;
    ks_getline(fp, &args->str); // the header
    int i;
    for (i=0; i<args->ncols - NFIXED; i++)
    {
        args->str.l = 0;
        ks_getline(fp, &args->str);
        char *p = args->str.s;
        int j = 0;
        while ( *p && j<9 ) 
        {
            if ( *p=='\t' ) j++;
            p++;
        }
        if ( !*p ) error("Could not parse the line, expected 10 fields: [%s] [%s]\n", args->str.s, p);
        for (j=NFIXED; j<args->ncols; j++)
            if ( !strcmp(args->colnames[j],p) ) break;
        if ( j==args->ncols ) continue;
        sscanf(args->str.s, "%u\t%u\t%u\t%le\t%le\t%le\t%le\t%le\t%le", &args->dists[j].nall, &args->dists[j].ngood, &args->dists[j].nmissing,
            &args->dists[j].good_min, &args->dists[j].good_max, &args->dists[j].all_min, &args->dists[j].all_max, &args->dists[j].scale_min, &args->dists[j].scale_max);
        if ( !args->ignore[j] && args->dists[j].scale_min==args->dists[j].scale_max ) error("The annotation %s does not look good, please leave it out\n", args->colnames[j]);
    }
    args->ngood = args->nall = INT_MAX;
    for (i=NFIXED; i<args->ncols; i++)
    {
        if ( !args->dists[i].nall && !args->dists[i].nmissing ) error("No extremes found for the annotation: %s\n", args->colnames[i]);
        if ( args->dists[i].nall < args->nall ) args->nall = args->dists[i].nall;
        if ( args->dists[i].ngood < args->ngood ) args->ngood = args->dists[i].ngood;
    }
    fclose(fp);
}

static void init_extra_annot(args_t *args, char *annot)
{
    int i;
    for (i=NFIXED; i<args->ncols; i++)
        if ( !strcmp(annot,args->colnames[i]) ) break;
    if ( i==args->ncols ) error("The annotation \"%s\" is not available.\n", annot);
    args->names = (char**) realloc(args->names, sizeof(char*)*(args->nann+1));
    args->names[ args->nann ] = strdup(annot);
    args->ignore[i] = 0;
    args->col2names[i] = args->nann;
    args->ann2cols[ args->nann  ] = i;
    args->nann++;
}
static void init_annots(args_t *args)
{
    args->file = hts_open(args->fname, "r", NULL);
    if ( !args->file ) error("Could not read %s\n", args->fname);
    hts_getline(args->file,'\n',&args->str);
    const char *exp = "# [1]CHROM\t[2]POS\t[3]MASK\t[4]REF\t[5]ALT";
    if ( args->str.s[0] != '#' ) error("Missing header line in %s, was vcf query run with -H?\n", args->fname);
    if ( strncmp(args->str.s,exp,strlen(exp)) ) 
    {
        args->str.s[strlen(exp)-1] = 0;
        error("Version mismatch? %s:\n\t[%s]\n\t[%s]\n", args->fname, args->str.s, exp);
    }

    int i, j;
    char **colnames = split_list(args->str.s, '\t', &args->ncols);
    if ( args->ncols >= 8*sizeof(int)-1 ) error("Fixme: Too many columns (%d), currently limited by max %d\n", args->ncols, 8*sizeof(int)-1);

    args->colnames  = (char**) malloc(sizeof(char*)*args->ncols);
    for (i=0; i<args->ncols; i++)
    {
        char *p = colnames[i];
        while (*p && *p!=']') p++;
        assert(*p);
        args->colnames[i] = strdup(p+1);
        free(colnames[i]);
    }
    free(colnames);
    // check that column names are unique
    for (i=0; i<args->ncols; i++)
        for (j=0; j<i; j++)
            if ( !strcmp(args->colnames[i],args->colnames[j]) ) error("Error: duplicate column names in %s [%s]\n", args->fname, args->colnames[i]);
    args->col2names = (int*) malloc(sizeof(int)*args->ncols);
    args->missing   = (int*) malloc(sizeof(int)*args->ncols);
    args->ignore    = (int*) malloc(sizeof(int)*args->ncols);
    args->vals      = (double*) malloc(sizeof(double)*args->ncols);
    args->raw_vals  = (double*) malloc(sizeof(double)*args->ncols);
    args->ann2cols  = (int*) malloc(sizeof(int)*args->ncols);
    for (i=0; i<args->ncols; i++)
    {
        args->col2names[i] = -1;
        args->ignore[i] = 1;
    }
    for (i=0; i<NFIXED; i++)
        args->ignore[i] = 0;

    if ( !args->annot_str )
    {
        // all annotations
        args->nann     = args->ncols - NFIXED;
        args->nann_som = args->nann;
        args->names    = (char**) malloc(sizeof(char*)*args->nann_som);
        for (i=NFIXED; i<args->ncols; i++)
        {
            args->col2names[i] = i-NFIXED;
            args->ignore[i] = 0;
            args->names[i-NFIXED] = strdup(args->colnames[i]);
            args->ann2cols[i-NFIXED] = i;
        }
        init_dists(args);
        return;
    }

    args->names     = split_list(args->annot_str, ',', &args->nann);
    args->nann_som  = args->nann;
    for (i=0; i<args->nann; i++)
    {
        for (j=NFIXED; j<args->ncols; j++)
            if ( !strcmp(args->colnames[j], args->names[i]) ) break;
        if ( j==args->ncols ) 
            error("The requested annotation \"%s\" not in %s\n", args->names[i], args->fname);
        if ( args->col2names[j]!=-1 )
            error("The annotation \"%s\" given multiple times?\n", args->names[i]);
        args->col2names[j] = i;
        args->ann2cols[i] = j;
        args->ignore[j] = 0;
    }
    init_dists(args);
}
static void destroy_annots(args_t *args)
{
    free(args->dists);
    if ( args->file ) hts_close(args->file);
    if ( args->str.m ) free(args->str.s);
    if ( args->nann ) 
    {
        destroy_list(args->names, args->nann);
        free(args->col2names);
        free(args->ann2cols);
    }
    int i;
    for (i=0; i<args->ncols; i++)
        free(args->colnames[i]);
    free(args->colnames);
    if ( args->missing ) free(args->missing);
    if ( args->ignore ) free(args->ignore);
    if ( args->vals ) free(args->vals);
    if ( args->raw_vals ) free(args->raw_vals);
}

static uint64_t failed_filters(filters_t *filt, double *vec)
{
    uint64_t failed = 0;
    int i, j, k = 0;
    for (i=0; i<filt->nann; i++)
    {
        if ( !filt->nfilt[i] ) continue;
        for (j=0; j<filt->nfilt[i]; j++)
        {
            switch (filt->filt[i][j].type) 
            {
                case FLT_BE: if ( vec[i] < filt->filt[i][j].value )  failed |= 1<<k; break;
                case FLT_BT: if ( vec[i] <= filt->filt[i][j].value ) failed |= 1<<k; break;
                case FLT_EQ: if ( vec[i] != filt->filt[i][j].value ) failed |= 1<<k; break;
                case FLT_LT: if ( vec[i] >= filt->filt[i][j].value ) failed |= 1<<k; break;
                case FLT_LE: if ( vec[i] > filt->filt[i][j].value )  failed |= 1<<k; break;
            }
            k++;
        }
    }
    return failed;
}
static void init_filters(args_t *args, filters_t *filts, char *str, int scale)
{
    filts->ntot  = 0;
    filts->nann  = args->nann;
    filts->filt  = (filter_t**) calloc(args->ncols-NFIXED,sizeof(filter_t*));
    filts->nfilt = (int*) calloc(args->ncols-NFIXED,sizeof(int));

    char *s, *e = str;
    args->str.l = 0;
    while ( *e )
    {
        if ( !isspace(*e) ) kputc(*e, &args->str);
        e++;
    }

    kstring_t left = {0,0,0}, right = {0,0,0}, tmp = {0,0,0};
    int type = 0;

    e = s = args->str.s;
    char *f = s;
    while ( *e )
    {
        if ( *e=='&' ) { s = ++e; continue; }
        if ( *e=='<' || *e=='>' || *e=='=' )
        {
            left.l = 0;
            kputsn(s, e-s, &left);
            f = s;
            s = e;
            while ( *e && (*e=='<' || *e=='>' || *e=='=') ) e++;
            if ( !*e ) error("Could not parse filter expression: %s\n", str);
            if ( e-s==2 )
            {
                if ( !strncmp(s,"==",2) ) type = FLT_EQ;
                else if ( !strncmp(s,"<=",2) ) type = FLT_LE;
                else if ( !strncmp(s,">=",2) ) type = FLT_BE;
                else error("Could not parse filter expression: %s\n", str);
            }
            else if ( e-s==1 )
            {
                switch (*s)
                {
                    case '>': type = FLT_BT; break;
                    case '<': type = FLT_LT; break;
                    case '=': type = FLT_EQ; break;
                    default: error("Could not parse filter expression: %s\n", str); break;
                }
            }
            else error("Could not parse filter expression: %s\n", str);
            s = e;
            while ( *e && *e!='&' ) e++;
            right.l = 0;
            kputsn(s, e-s, &right);

            char *ann = NULL;
            int i;
            for (i=NFIXED; i<args->ncols; i++)
            {
                if ( !strcmp(args->colnames[i],left.s) ) { s = right.s; ann = left.s; break; }
                if ( !strcmp(args->colnames[i],right.s) ) { s = left.s; ann = right.s; type *= -1; break; }
            }
            if ( i==args->ncols ) error("No such annotation is available: %s\n", str);
            if ( args->col2names[i]==-1 )
            {
                init_extra_annot(args, ann);
                filts->nann = args->nann;
            }
            tmp.l = 0;
            ksprintf(&tmp, "%s\t", args->colnames[i]); kputsn(f, e-f, &tmp);

            i = args->col2names[i];
            filts->nfilt[i]++;
            filts->ntot++;
            filts->filt[i] = (filter_t*) realloc(filts->filt[i],sizeof(filter_t)*filts->nfilt[i]);
            filter_t *filt = &filts->filt[i][filts->nfilt[i]-1];
            filt->type = type;
            if ( sscanf(s,"%le",&filt->value)!=1 ) error("Could not parse filter expression: %s\n", str);
            if ( scale )
                filt->value = scale_value(&args->dists[args->ann2cols[i]], filt->value);
            filt->desc = strdup(tmp.s);
            filts->flt_mask_desc = (char**) realloc(filts->flt_mask_desc,sizeof(char*)*filts->ntot);
            char *t = filt->desc; while ( *t && *t!='\t' ) t++; t++;
            filts->flt_mask_desc[filts->ntot-1] = t;
            s = e;
            continue;
        }
        e++;
    }
    if ( tmp.m ) free(tmp.s);
    if ( left.m ) free(left.s);
    if ( right.m ) free(right.s);
    if ( filts->ntot > sizeof(uint64_t)-1 ) error("Uh, too many hard-filters: %d\n", filts->ntot);
}
static void destroy_filters(filters_t *filt)
{
    if ( !filt->nfilt ) return;
    int i;
    for (i=0; i<filt->nann; i++)
    {
        if ( filt->filt[i] ) 
        {
            if ( filt->filt[i]->desc ) free(filt->filt[i]->desc);
            free(filt->filt[i]);
        }
    }
    free(filt->filt);
    free(filt->nfilt);
    free(filt->flt_mask_desc);
}

#if 0
static void som_plot(som_t *som, char *bname, int do_plot)
{
    char *fname;
    FILE *fp = open_file(&fname,"w","%s.py", bname);
    fprintf(fp,
            "import matplotlib as mpl\n"
            "mpl.use('Agg')\n"
            "import matplotlib.pyplot as plt\n"
            "\n"
            "dat = [\n"
           );
    int i,j;
    double *val = som->c;
    for (i=0; i<som->nbin; i++)
    {
        fprintf(fp,"[");
        for (j=0; j<som->nbin; j++)
        {
            if ( j>0 ) fprintf(fp,",");
            fprintf(fp,"%e", *val);
            val++;
        }
        fprintf(fp,"],\n");
    }
    fprintf(fp,
            "]\n"
            "fig = plt.figure()\n"
            "ax1 = plt.subplot(111)\n"
            "im1 = ax1.imshow(dat, interpolation='nearest')\n"
            "fig.colorbar(im1)\n"
            "plt.savefig('%s.png')\n"
            "plt.close()\n"
            "\n", bname
           );
    fclose(fp);
    if ( do_plot ) py_plot(fname);
    free(fname);
}
#endif
static void som_train(som_t *som, double *vec)
{
    // randomly select one of the maps to train
    int jsom = som->nsom==1 ? 0 : (double)som->nsom * random()/RAND_MAX;
    assert( jsom<som->nsom );   // not sure if RAND_MAX is inclusive

    // find the best matching unit: a node with minimum distance from the input vector
    double *ptr = som->w + jsom*som->nbin*som->nbin*som->kdim;
    double *cnt = som->c + jsom*som->nbin*som->nbin;
    double min_dist = HUGE_VAL;
    int i, j, k, imin = 0, jmin = 0;
    for (i=0; i<som->nbin; i++)
    {
        for (j=0; j<som->nbin; j++)
        {
            double dist = 0;
            for (k=0; k<som->kdim; k++)
                dist += (vec[k] - ptr[k]) * (vec[k] - ptr[k]);
            if ( dist < min_dist )
            {
                min_dist = dist;
                imin = i;
                jmin = j;
            }
            ptr += som->kdim;
        }
    }

    double t = som->t[jsom]++ * som->nsom;
    double radius = som->nbin * exp(-t/som->nt);
    radius *= radius;
    double learning_rate = som->learn * exp(-t/som->nt);

    // update the weights
    ptr = som->w;
    for (i=0; i<som->nbin; i++)
    {
        for (j=0; j<som->nbin; j++)
        {
            double dist = (i-imin)*(i-imin) + (j-jmin)*(j-jmin);
            if ( dist <= radius ) 
            {
                double influence = exp(-dist*dist*0.5/radius) * learning_rate;
                for (k=0; k<som->kdim; k++)
                    ptr[k] += influence * (vec[k] - ptr[k]);
                *cnt += influence;
            }
            ptr += som->kdim;
            cnt++;
        }
    }
}
static double som_calc_dist(som_t *som, double *vec)
{
    int i, j, k, js;
    double *cnt = som->c, *ptr = som->w;
    // double avg_dist = 0;    // not working great
    // double max_dist = 0;        // not working great either
    double min_som_dist = HUGE_VAL;
    for (js=0; js<som->nsom; js++)
    {
        double min_dist = HUGE_VAL;
        for (i=0; i<som->nbin; i++)
        {
            for (j=0; j<som->nbin; j++)
            {
                if ( *cnt >= som->th )
                {
                    double dist = 0;
                    for (k=0; k<som->kdim; k++)
                        dist += (vec[k] - ptr[k]) * (vec[k] - ptr[k]);

                    if ( dist < min_dist )
                        min_dist = dist;
                }
                cnt++;
                ptr += som->kdim;
            }
        }
        if ( min_som_dist > min_dist ) min_som_dist = min_dist;
        //if ( max_dist < min_dist ) max_dist = min_dist;
        //avg_dist += min_dist;
    }
    //return avg_dist/som->nsom;
    //return max_dist;
    return min_som_dist;
}
static void som_norm(som_t *som)
{
    int i, j, n2 = som->nbin*som->nbin;
    for (j=0; j<som->nsom; j++)
    {
        double max = 0;
        double *c = som->c + j*n2;
        for (i=0; i<n2; i++) 
            if ( max < som->c[i] ) max = c[i];
        if ( max )
            for (i=0; i<n2; i++) c[i] /= max;
    }
}
static void som_destroy(som_t *som)
{
    free(som->t);
    free(som->w);
    free(som->c);
    free(som);
}
static som_t *som_init1(args_t *args, int kdim, som_t *template)
{
    som_t *som = (som_t *) calloc(1,sizeof(som_t));
    *som = *template;
    som->kdim   = kdim;
    som->w      = (double*) malloc(sizeof(double) * som->kdim * som->nbin * som->nbin * som->nsom);
    som->c      = (double*) calloc(som->nbin*som->nbin*som->nsom, sizeof(double));
    som->t      = (int*) calloc(som->nsom,sizeof(int));
    if ( !som->nt || som->nt > args->ngood ) som->nt = args->ngood;
    return som;
}
static void som_init_rand(som_t *som, int seed)
{
    if ( seed>=0 ) srandom(seed);
    int i, n3 = som->nbin * som->nbin * som->kdim * som->nsom;
    for (i=0; i<n3; i++)
        som->w[i] = (double)random()/RAND_MAX;
}
static void som_init(args_t *args)
{
    int i, j;
    som_t *good_som = som_init1(args, args->nann_som, &args->som);
    som_init_rand(good_som, args->rand_seed);
    args->good_som = good_som;
    int ngood_fixed_max = good_som->nt * (1-args->nt_learn_frac);
    int ngood_learn_max = good_som->nt * args->nt_learn_frac;
    int ngood_vals_fixed = 0, ngood_vals_learn = 0;
    double *good_vals_fixed = (double*) malloc(sizeof(double)*ngood_fixed_max*args->nann_som);
    double *good_vals_learn = (double*) malloc(sizeof(double)*ngood_learn_max*args->nann_som);

    srandom(args->rand_seed);
    annots_reader_reset(args);
    while ( annots_reader_next(args) )
    {
        // To be changed: pass the default value on command line or in a file
        //
        // Note well: filtering and training requires that all annotations used in
        //  hard-filtering and learning are present, even when not used in SOM filtering. 
        //  So far the decision was that it is the responsibility of the caller
        //  to supply meaningful default values if the annotation is not
        //  applicable for a site (for example some tests cannot be performed
        //  on a pure HOM site). This may not be an optimal solution and it
        //  might be better to treat the missing values here, possibly with a
        //  user switch to decide whether the missing values by default pass or
        //  fail hard filters.
        //
        if ( args->nset != args->nann ) continue;

        if ( !IS_GOOD(args->mask) )
        {
            if ( !args->filt_learn.nfilt ) continue;    // this is supervised learning, ignore non-training sites
            if ( !ngood_learn_max ) continue;
            if ( failed_filters(&args->filt_learn, args->vals) ) continue;
            if ( ngood_vals_learn < ngood_learn_max )
            {
                i = ngood_vals_learn;
                ngood_vals_learn++;
            }
            else
                i = (double)(ngood_learn_max-1)*random()/RAND_MAX;
            for (j=0; j<args->nann_som; j++)
                good_vals_learn[i*args->nann_som + j] = args->vals[j];
        }
        else
        {
            if ( !ngood_fixed_max ) continue;
            if ( ngood_vals_fixed < ngood_fixed_max )
            {
                i = ngood_vals_fixed;
                ngood_vals_fixed++;
            }
            else
                i = (double)(ngood_fixed_max-1)*random()/RAND_MAX;
            for (j=0; j<args->nann_som; j++)
                good_vals_fixed[i*args->nann_som + j] = args->vals[j];
        }
    }
    if ( ngood_vals_learn + ngood_vals_fixed < good_som->nt ) good_som->nt = ngood_vals_learn + ngood_vals_fixed;
    fprintf(stderr,"Selected %d training vectors: %d from good sites, %d from -l sites.\n",good_som->nt,ngood_vals_fixed,ngood_vals_learn);

    for (i=0; i<ngood_vals_fixed; i++) som_train(good_som, &good_vals_fixed[i*args->nann_som]);
    for (i=0; i<ngood_vals_learn; i++) som_train(good_som, &good_vals_learn[i*args->nann_som]);
    free(good_vals_fixed);
    free(good_vals_learn);

    som_norm(good_som);

    // char *fname = msprintf("%s.som", args->out_prefix);
    // som_plot(good_som, fname, args->do_plot);
    // free(fname);
}

static void init_data(args_t *args)
{
    fprintf(stderr,"Initializing and training...\n");
    if ( !args->out_prefix ) args->out_prefix = strdup(args->fname);
    else
        args->out_prefix = msprintf("%s/annots", args->out_prefix);
    if ( !args->out_prefix ) error("Missing the -o option.\n");
    init_annots(args);
    if ( args->learning_filters ) init_filters(args, &args->filt_learn, args->learning_filters, 0);
    som_init(args);
    if (args->ref_fname) args->indel_ctx = indel_ctx_init(args->ref_fname);
}
static void destroy_data(args_t *args)
{
    destroy_filters(&args->filt_learn);
    destroy_annots(args);
    free(args->out_prefix);
    som_destroy(args->good_som);
    //som_destroy(args->bad_som);
    if (args->indel_ctx) indel_ctx_destroy(args->indel_ctx);
}

/*
    SNPs:   transversion=0, transition=1
    Indels: repeat-inconsistent=0, repeat-consistent=1, not-applicable=2
*/
static int determine_variant_class(args_t *args)
{
    if ( args->filt_type==VCF_SNP )
        return abs(bcf_acgt2int(args->ref[0])-bcf_acgt2int(args->alt[0])) == 2 ? 1 : 0;

    if ( !args->indel_ctx ) return 2; 

    int nrep, nlen, ndel;
    ndel = indel_ctx_type(args->indel_ctx, args->chr, args->pos, args->ref, args->alt, &nrep, &nlen);
    if ( nlen<=1 || nrep<=1 ) return 2;

    if ( abs(ndel) % nlen ) return 0;
    return 1;
}

static void eval_filters(args_t *args)
{
    init_data(args);

    char *fname; 
    open_file(&fname,NULL,"%s.sites.gz", args->out_prefix);
    BGZF *file = bgzf_open(fname, "w");

    // Calculate scores
    kstring_t str = {0,0,0};
    kputs("# [1]score\t[2]variant class\t[3]filter mask, good(&1)\t[4]chromosome\t[5]position\n", &str);
    bgzf_write(file, str.s, str.l);
    str.l = 0;

    fprintf(stderr,"Classifying...\n");
    annots_reader_reset(args);
    int ngood = 0, nall = 0;
    double max_dist = args->good_som->kdim;
    while ( annots_reader_next(args) )
    {
        if ( args->nset != args->nann ) continue;

        double dist = som_calc_dist(args->good_som, args->vals);

        if ( IS_GOOD(args->mask) ) ngood++;
        nall++;

        str.l = 0;
        double score = dist/max_dist;
        int class = determine_variant_class(args);
        ksprintf(&str, "%le\t%d\t%d\t%s\t%d\n", score, class, IS_GOOD(args->mask) ? 1 : 0, args->chr, args->pos);
        bgzf_write(file, str.s, str.l);
    }
    bgzf_close(file);
    free(fname);

    // Evaluate. The segregating metric is ts/tv for SNPs and repeat-consistency for indels,
    //  see also determine_variant_class.
    fprintf(stderr,"Evaluating...\n");
    int ngood_read = 0, nall_read = 0, nclass[3] = {0,0,0}, nclass_novel[3] = {0,0,0};
    double prev_metric = -1;
    args->str.l = 0;
    ksprintf(&args->str, "gunzip -c %s.sites.gz | cut -f1-3 | sort -k1,1g ", args->out_prefix);
    if ( args->sort_args )  kputs(args->sort_args, &args->str);
    FILE *fp = popen(args->str.s, "r");
    if ( !fp ) error("Could not run \"%s\": %s\n", args->str.s, strerror(errno));
    FILE *out = open_file(NULL,"w","%s.tab", args->out_prefix);

    double metric_th = 0.005;
    if ( args->filt_type==VCF_SNP )
        fprintf(out,"# [1]ts/tv (all)\t[2]nAll\t[3]sensitivity\t[4]ts/tv (novel)\t[5]threshold\n");
    else
        fprintf(out,"# [1]repeat consistency (all)\t[2]nAll\t[3]sensitivity\t[4]repeat consistency (novel)\t[5]threshold\n");

    // Version and command line string
    str.l = 0;
    ksprintf(&str,"# bcftools_somVersion=%s\n", bcftools_version());
    fputs(str.s, out);
    str.l = 0;
    ksprintf(&str,"# bcftools_somCommand=%s", args->argv[0]);
    int i;
    for (i=1; i<args->argc; i++) ksprintf(&str, " %s", args->argv[i]);
    kputc('\n', &str);
    fputs(str.s, out);

    while ( ks_getline(fp, &args->str) )
    {
        if ( args->str.s[0]=='#' ) 
        {
            args->str.l = 0;
            continue;
        }

        // SCORE
        double dist = atof(args->str.s);
        char *t = column_next(args->str.s, '\t'); 
        if ( !*t ) error("Could not parse score: [%s]\n", args->str.s);
        // CLASS
        int class = strtol(t, NULL, 10);
        t = column_next(t+1, '\t');  
        if ( !*t ) error("Could not parse POS: [%s]\n", args->str.s);
        // IS_GOOD
        int mask = strtol(t, NULL, 10);
        args->str.l = 0;

        nall_read++;
        nclass[class]++;
        if ( mask&1 ) ngood_read++;
        else if ( ngood ) nclass_novel[class]++;

        // Do not output anything until 10% of sites is scanned
        if ( (double)nall_read/nall < 0.1 ) continue;

        double metric = args->filt_type==VCF_SNP ? (double)nclass[1]/nclass[0] : (double)nclass[1]/(nclass[1]+nclass[0]);
        if ( prev_metric==-1 || fabs(prev_metric - metric)>metric_th )
        {
            double metric_novel;
            if ( !nclass_novel[0] ) metric_novel = 0;
            else if ( args->filt_type==VCF_SNP ) metric_novel = (double)nclass_novel[1]/nclass_novel[0];
            else metric_novel = (double)nclass_novel[1]/(nclass_novel[1]+nclass_novel[0]);
            fprintf(out, "%.3f\t%d\t%.2f\t%.3f\t%le\n", metric, nall_read, ngood ? (double)100.*ngood_read/ngood : 0, metric_novel, dist);
            prev_metric = metric;
        }
    }
    if ( str.m ) free(str.s);
    if ( fclose(out)!=0 ) error("An error occurred while processing %s.tab\n", args->out_prefix);
    if ( fclose(fp)!=0 ) error("An error occurred while processing gunzip -c %s.sites.gz: %s\n", args->out_prefix);
    destroy_data(args);
}

typedef struct
{
    htsFile *file;
    hts_itr_t *itr;
    tbx_t *tbx;
    kstring_t str;
    double score;
    int rid, pos;
    uint64_t flt_mask;
}
site_t;
static void destroy_site(site_t *site)
{
    if ( site->str.m ) free(site->str.s); 
    if ( site->file ) hts_close(site->file);
    if ( site->itr ) tbx_itr_destroy(site->itr);
    if ( site->tbx ) tbx_destroy(site->tbx);
    free(site);
}
static site_t *init_site(char *fname, char *region)
{
    site_t *site = (site_t*) calloc(1,sizeof(site_t));
    site->file = hts_open(fname, region ? "rb" : "r", NULL);
    if ( !site->file ) error("Error: could not read %s\n", fname);
    if ( region ) 
    {
        site->tbx = tbx_index_load(fname);
        if ( !site->tbx )
        {
            tbx_conf_t conf = { 0, 4, 5, 0, '#', 0 };
            tbx_index_build(fname,0,&conf);
            site->tbx = tbx_index_load(fname);
        }
        if ( !site->tbx ) error("Error: could not load index of %s\n", fname);
        site->itr = tbx_itr_querys(site->tbx,region);
        if ( !site->itr ) error("Error: could not init itr of %s, is the tabix index broken?\n", fname);
    }
    return site;
}
static int sync_site(bcf_hdr_t *hdr, bcf1_t *line, site_t *site, int type)
{
    while (1)
    {
        if ( !site->str.l )
        {
            // no site in the buffer
            if ( site->itr )
            {
                if ( tbx_itr_next(site->file, site->tbx, site->itr, &site->str) < 0 ) return 0;
            }
            else
                if ( hts_getline(site->file, '\n', &site->str) <= 0 ) return 0;
            
            if ( site->str.s[0]=='#' ) 
            {
                site->str.l = 0;
                continue;
            }

            // SCORE
            site->score = atof(site->str.s);
            char *t = column_next(site->str.s, '\t'); 
            if ( !*t ) error("Could not parse SCORE: [%s]\n", site->str.s);
            // IS_TS
            t = column_next(t+1, '\t');  
            if ( !*t ) error("Could not parse variant class: [%s]\n", site->str.s);
            // FILTER MASK
            site->flt_mask = strtol(t+1, NULL, 10);
            t = column_next(t+1, '\t');  
            if ( !*t ) error("Could not parse FILTER MASK: [%s]\n", site->str.s);
            // CHR
            char *p = t+1;
            t = column_next(t+1, '\t');  
            *t = 0;
            site->rid = bcf_name2id(hdr, p);
            if ( site->rid<0 ) error("The chrom \"%s\" not in the header?\n", p);
            // POS
            site->pos = strtol(t+1, NULL, 10);
        }
        if ( !(line->d.var_type & type) ) return 0;
        if ( line->pos+1 == site->pos && line->rid == site->rid )
        {
            site->str.l = 0;
            return 1;
        }
        if ( line->rid != site->rid ) 
            error("Warning: The sites file positioned on different chromosome (%s vs %s), did you want to run with the -r option?\n", 
                    hdr->id[BCF_DT_CTG][site->rid].key,hdr->id[BCF_DT_CTG][line->rid].key);
        if ( line->pos+1 < site->pos ) return 0;
        if ( line->pos+1 > site->pos && line->rid == site->rid ) 
            error("The the sites file is out of sync, was it created from different VCF? The conflicting site is %s:%d vs %d\n", 
                hdr->id[BCF_DT_CTG][line->rid].key, site->pos,line->pos+1);
        break;
    }
    return 0;
}


static void apply_filters(args_t *args)
{
    // Init files
    site_t *snp = NULL, *indel = NULL;
    if ( args->snp_th >= 0 ) snp = init_site(args->snp_sites_fname, args->region);
    if ( args->indel_th >= 0 ) indel = init_site(args->indel_sites_fname, args->region);

    bcf_srs_t *sr = bcf_sr_init();
    if ( args->region && bcf_sr_set_regions(sr, args->region)<0 )
        error("Failed to set the region: %s\n", args->region);
    if ( !bcf_sr_add_reader(sr, args->fname) ) error("Failed to open or the file is not indexed: %s\n", args->fname);
    bcf_hdr_t *hdr = sr->readers[0].header;

    // Add header lines
    kstring_t str = {0,0,0};
    kputs("##FILTER=<ID=FailSOM,Description=\"Failed SOM filter (lower is better):", &str);
    if ( snp )
    {
        ksprintf(&str, " SNP cutoff %e", args->snp_th);
        if ( indel ) kputc(';', &str);
    }
    if ( indel )
        ksprintf(&str, " INDEL cutoff %e", args->indel_th);
    kputs(".\">", &str);
    bcf_hdr_append(hdr, str.s); free(str.s); str.m = str.l = 0;
    bcf_hdr_append(hdr, "##INFO=<ID=FiltScore,Number=1,Type=Float,Description=\"SOM Filtering Score\">");
    bcf_hdr_append_version(hdr, args->argc, args->argv, "bcftools_som");
    bcf_hdr_fmt_text(hdr);
    int flt_pass = bcf_id2int(hdr, BCF_DT_ID, "PASS");
    int flt_fail = bcf_id2int(hdr, BCF_DT_ID, "FailSOM");

    htsFile *out = hts_open("-", hts_bcf_wmode(args->output_type), 0);
    vcf_hdr_write(out, hdr);

    while ( bcf_sr_next_line(sr) ) 
    {
        bcf1_t *line = sr->readers[0].buffer[0];
        bcf_unpack(line, BCF_UN_INFO);
        bcf_set_variant_types(line);
        if ( snp && sync_site(hdr, line, snp, VCF_SNP) )
        {
            float tmp = snp->score;
            bcf1_update_info_float(hdr, line, "FiltScore", &tmp, 1);
            bcf1_update_filter(hdr, line, snp->score <= args->snp_th ? &flt_pass : &flt_fail, 1);
            vcf_write1(out, hdr, line);
            continue;
        }
        if ( indel && sync_site(hdr, line, indel, VCF_INDEL) )
        {
            float tmp = indel->score;
            bcf1_update_info_float(hdr, line, "FiltScore", &tmp, 1);
            bcf1_update_filter(hdr, line, indel->score <= args->indel_th ? &flt_pass : &flt_fail, 1);
            vcf_write1(out, hdr, line);
            continue;
        }
        if ( args->unset_unknowns ) bcf1_update_filter(hdr, line, NULL, 0);
        vcf_write1(out, hdr, line);
    }
    if ( snp ) destroy_site(snp);
    if ( indel ) destroy_site(indel);
    hts_close(out);
    bcf_sr_destroy(sr);
}

void set_sort_args(args_t *args)
{
    char *env = getenv("SORT_ARGS");
    if ( !env ) return;
    char *t = env;
    while ( *t && (*t==' ' || *t=='-' || isdigit(*t) || isalpha(*t) || *t=='/') ) t++;
    if ( *t ) error("Could not validate SORT_ARGS=\"%s\"\n", env);
    args->sort_args = env;
    fprintf(stderr,"Detected SORT_ARGS=\"%s\"\n", env);
}

static void usage(void)
{
	fprintf(stderr, "About:   SOM (Self-Organizing Map) filtering.\n");
	fprintf(stderr, "Usage:   bcftools som [options] <annots.tab.gz>\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "    -a, --annots <list>                            list of annotations (default: use all annotations)\n");
	fprintf(stderr, "    -f, --fixed-filter <expr>                      list of fixed threshold filters to apply (absolute values, e.g. 'QUAL>4')\n");
	fprintf(stderr, "    -F, --fasta-ref <file>                         faidx indexed reference sequence file, required to determine INDEL type\n");
	fprintf(stderr, "    -g, --good-mask <mask>                         mask to recognise good variants in annots.tab.gz [010]\n");
	fprintf(stderr, "    -i, --indel-threshold <float> <file>           filter INDELs at the given threshold using the supplied sites file\n");
	fprintf(stderr, "    -l, --learning-filters <expr>                  filters for selecting training sites (values scaled to interval [0-1])\n");
	fprintf(stderr, "    -m, --map-parameters <int,float,float,int>     number of bins, learning constant, BMU threshold, nsom [20,0.1,0.2,1]\n");
	fprintf(stderr, "    -n, --ntrain-sites <int,float>                 number of training sites and the fraction of -l sites [0,0]\n");
    fprintf(stderr, "    -o, --output-type <b|u|z|v>                    'b' compressed BCF; 'u' uncompressed BCF; 'z' compressed VCF; 'v' uncompressed VCF [v]\n");
	fprintf(stderr, "    -p, --output-prefix <string>                   prefix of output files\n");
	fprintf(stderr, "    -r, --region <chr|chr:from-to>                 apply filtering in this region only\n");
	fprintf(stderr, "    -R, --random-seed <int>                        random seed, 0 for time() [1]\n");
	fprintf(stderr, "    -s, --snp-threshold <float> <file>             filter SNPs at the given threshold using the supplied sites file\n");
	fprintf(stderr, "    -t, --type <SNP|INDEL>                         variant type to filter [SNP]\n");
	fprintf(stderr, "    -u, --unset-unknowns                           set FILTER of sites which are not present in annots.tab.gz to \".\"\n");
	fprintf(stderr, "\nExample:\n");
	fprintf(stderr, "   # 1) This step extracts annotations from the VCF and creates a compressed tab-delimited file\n");
    fprintf(stderr, "   #    which tends to be smaller and much faster to parse (several passes through the data are\n");
    fprintf(stderr, "   #    required). The second VCF is required only for supervised learning, SNPs and indels can\n");
    fprintf(stderr, "   #    be given in separate files.\n");
	fprintf(stderr, "   bcftools query -Ha QUAL,Annot1,Annot2,... target.vcf.gz training.vcf.gz | bgzip -c > annots.tab.gz\n");
	fprintf(stderr, "\n");
    fprintf(stderr, "   # 2) This step is usually run multiple times to test which annotations and\n");
    fprintf(stderr, "   #    parameters give the best results. Here the input values are normalized,\n");
    fprintf(stderr, "   #    SOM model trained and filtering scores calculated for all sites.\n");
    fprintf(stderr, "   #    Tab-delimited output files are then created to help decide about\n");
    fprintf(stderr, "   #    best thresholds to reach the desired sensitivity, transition/transversion\n");
    fprintf(stderr, "   #    ratio (SNPs) and repeat-consistency value (indels). \n");
    fprintf(stderr, "   #    Note that: \n");
    fprintf(stderr, "   #       - SNPs and INDELs are done separately (-t)\n");
    fprintf(stderr, "   #       - The -l option can be used also in presence of the truth set (training.vcf.gz in the example above)\n");
    fprintf(stderr, "   #       - Without the -a option, all annotations from annots.tab.gz are used\n");
	fprintf(stderr, "   bcftools filter annots.tab.gz -p prefix -l'QUAL>0.6' -a Annot1,Annot2,Annot3\n");
	fprintf(stderr, "   bcftools filter annots.tab.gz -p prefix -l'QUAL>0.6' -a Annot2,Annot4 -t INDEL\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "   # 3) Choose threshold in prefix/annots.tab and apply with -i and -s. The INFO\n");
	fprintf(stderr, "   #    tag FiltScore is added and sites failing the SOM filter have the FailSOM filter set.\n");
	fprintf(stderr, "   bcftools filter target.vcf.gz -u -s 1.054277e-02 snps/annots.sites.gz -i 5.012345e-04 indels/annots.sites.gz -oz > filtered.vcf.gz\n");
	fprintf(stderr, "\n");
	exit(1);
}

int main_vcfsom(int argc, char *argv[])
{
	int c;
	args_t *args    = (args_t*) calloc(1,sizeof(args_t));
	args->argc      = argc; args->argv = argv;
    args->lo_pctl   = 0.1;
    args->hi_pctl   = 99.9;
    args->som.nbin  = 20;
    args->som.learn = 0.1;
    args->som.th    = 0.2;
    args->som.nsom  = 1;
    args->scale     = 1;
    args->filt_type = VCF_SNP;
    args->snp_th    = -1;
    args->indel_th  = -1;
    args->rand_seed = 1;
    args->good_mask = parse_mask("010");

	static struct option loptions[] = 
	{
		{"help",0,0,'h'},
		{"annots",1,0,'a'},
		{"output-prefix",1,0,'p'},
		{"output-type",1,0,'o'},
		{"map-params",1,0,'m'},
		{"create-plots",0,0,'p'},
		{"fixed-filter",1,0,'f'},
		{"fasta-ref",1,0,'F'},
		{"type",1,0,'t'},
		{"ntrain-sites",1,0,'n'},
		{"learning-filters",1,0,'l'},
		{"snp-threshold",1,0,'s'},
		{"indel-threshold",1,0,'i'},
		{"random-seed",1,0,'R'},
		{"region",1,0,'r'},
		{"good-mask",1,0,'g'},
		{"unset-unknowns",1,0,'u'},
		{0,0,0,0}
	};
	while ((c = getopt_long(argc, argv, "ho:a:s:i:p:f:t:n:l:m:r:R:g:F:u",loptions,NULL)) >= 0) {
		switch (c) {
            case 'o': 
                switch (optarg[0]) {
                    case 'b': args->output_type = FT_BCF_GZ; break;
                    case 'u': args->output_type = FT_BCF; break;
                    case 'z': args->output_type = FT_VCF_GZ; break;
                    case 'v': args->output_type = FT_VCF; break;
                    default: error("The output type \"%s\" not recognised\n", optarg);
                }
                break;
			case 'u': args->unset_unknowns = 1; break;
			case 't': 
                {
                    if ( !strcasecmp("SNP",optarg) ) args->filt_type = VCF_SNP;  
                    else if ( !strcasecmp("INDEL",optarg) ) args->filt_type = VCF_INDEL;
                    else error("The variant type \"%s\" not recognised.\n", optarg);
                    break;
                }
			case 'a': args->annot_str = optarg; break;
			case 'F': args->ref_fname = optarg; break;
			case 'n': 
                if (sscanf(optarg,"%d,%le",&args->som.nt,&args->nt_learn_frac)!=2) error("Could not parse -n %s\n", optarg); 
                if ( args->nt_learn_frac >1 ) args->nt_learn_frac *= 0.01;
                break;
			case 'g': args->good_mask = parse_mask(optarg); break;
			case 's': args->snp_th = atof(optarg); args->snp_sites_fname = argv[optind++]; break;
			case 'i': args->indel_th = atof(optarg); args->indel_sites_fname = argv[optind++]; break;
			case 'p': args->out_prefix = optarg; break;
			case 'R': args->rand_seed = atoi(optarg); break;
			case 'r': args->region = optarg; break;
            case 'l': args->learning_filters = optarg; break;
			case 'm': if (sscanf(optarg,"%d,%le,%le,%d",&args->som.nbin,&args->som.learn,&args->som.th,&args->som.nsom)!=4) error("Could not parse --SOM-params %s\n", optarg); break;
			case 'h': 
			case '?': usage();
			default: error("Unknown argument: %s\n", optarg);
		}
	}

    if ( !args->rand_seed ) args->rand_seed = time(NULL);
    if ( argc!=optind+1 ) usage();
    args->fname = argv[optind];
    if ( args->snp_th<0 && args->indel_th<0 )
    {
        fprintf(stderr,"Random seed %d\n", args->rand_seed);
        if ( args->region ) error("The -r option is to be used with -s or -i only.\n");
        if ( args->filt_type==VCF_INDEL && !args->ref_fname ) error("Expected the -F parameter with -t INDEL\n");
        set_sort_args(args);
        eval_filters(args);
    }
    else
        apply_filters(args);
	free(args);
	return 0;
}

