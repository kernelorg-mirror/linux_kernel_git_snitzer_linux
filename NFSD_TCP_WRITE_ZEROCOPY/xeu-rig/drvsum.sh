#!/bin/bash
# drvsum.sh <sec.tsv> [bucket seconds]: each drive's mean GB/s by bucket, its lowest second and writes/s
awk -v B=${2:-30} 'NR>1{for(i=2;i<=NF;i+=3){n=(i-2)/3; if(min[n]==""||$i<min[n])min[n]=$i; c=int((NR-2)/B); a[n,c]+=$i; k[n,c]++; w[n]+=$(i+2); if(c>C)C=c}} NR==1{for(i=2;i<=NF;i+=3){split($i,x,"_"); nm[(i-2)/3]=x[1]; N=(i-2)/3}} END{for(n=0;n<=N;n++){printf "%s min %.2f wps %d  %ds-means:",nm[n],min[n],w[n]/(NR-1),B; for(c=0;c<=C;c++)if(k[n,c])printf " %.1f",a[n,c]/k[n,c]; print ""}}' $1
