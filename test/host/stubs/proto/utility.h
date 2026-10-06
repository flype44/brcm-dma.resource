/* Host stand-in of utility.library's NextTagItem(): the same rules (TAG_DONE ends, TAG_IGNORE is skipped, TAG_SKIP skips ti_Data
   tags, TAG_MORE goes on with another list). On the Amiga the real function of the ROM is called. */
static inline struct TagItem *NextTagItem(struct TagItem **list)
{
    struct TagItem *t;

    while ((t = *list) != NULL)
    {
        switch (t->ti_Tag)
        {
            case TAG_DONE:   *list = NULL; return NULL;
            case TAG_IGNORE: *list = t + 1; break;
            case TAG_MORE:   *list = (struct TagItem *)(uintptr_t)t->ti_Data; break;
            case TAG_SKIP:   *list = t + 1 + t->ti_Data; break;
            default:         *list = t + 1; return t;
        }
    }

    return NULL;
}
