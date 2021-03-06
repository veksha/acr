#include "ballistics.h"

float srayclip(const vec &o, const vec &ray, vec *surface = NULL)
{
    float dist = sraycube(o, ray, surface);
    vec to = ray;
    to.mul(dist).add(o);
    bool collided = false;
    vec end;
    loopv(sents)
    {
        if (sents[i].type != CLIP /*&& sents[i] != MAPMODEL*/) continue;
        entity &e = sents[i];
        // attr1, attr2, attr3, attr4
        // elevation, xrad, yrad, height
        if (intersectbox(vec(e.x, e.y, getblockfloor(getmaplayoutid(e.x, e.y)) + e.attr1 + e.attr4 / 2), vec(max(0.1f, (float)e.attr2), max(0.1f, (float)e.attr3), max(0.1f, e.attr4 / 2.f)), o, to, &end))
        {
            to = end;
            collided = true;
            if (surface)
            {
                *surface = vec(0, 0, 0);
                // which surface did it hit?
            }
        }
    }
    return collided ? to.dist(o) : dist;
}

// trace a shot
void straceShot(const vec &from, vec &to, vec *surface = NULL)
{
    vec tracer(to);
    tracer.sub(from).normalize();
    const float dist = srayclip(from, tracer, surface);
    to = tracer.mul(dist - .1f).add(from);
}

// normal shots (ray through sphere and cylinder check)
static inline int hitplayer(const vec &from, float yaw, float pitch, const vec &to, const vec &target, const vec &head, vec *end = NULL)
{
    float dist;
    // intersect head
    if (!head.iszero() && intersectsphere(from, to, head, HEADSIZE, dist))
    {
        if (end) (*end = to).sub(from).mul(dist).add(from);
        return HIT_HEAD;
    }
    float y = yaw*RAD, p = (pitch / 4 + 90)*RAD, c = cosf(p);
    vec bottom(target), top(sinf(y)*c, -cosf(y)*c, sinf(p));
    bottom.z -= PLAYERHEIGHT;
    top.mul(PLAYERHEIGHT/* + d->aboveeye*/).add(bottom); // space above shoulders removed
    // torso
    bottom.sub(top).mul(TORSOPART).add(top);
    if (intersectcylinder(from, to, bottom, top, PLAYERRADIUS, dist))
    {
        if (end) (*end = to).sub(from).mul(dist).add(from);
        return HIT_TORSO;
    }
    // restore to body
    bottom.sub(top).div(TORSOPART).add(top);
    // legs
    top.sub(bottom).mul(LEGPART).add(bottom);
    if (intersectcylinder(from, to, bottom, top, PLAYERRADIUS, dist)){
        if (end) (*end = to).sub(from).mul(dist).add(from);
        return HIT_LEG;
    }
    return HIT_NONE;
}

// apply spread
void applyspread(const vec &from, vec &to, int spread, float factor){
    if (spread <= 1) return;
#define RNDD (rnd(spread)-spread/2.f)*factor
    vec r(RNDD, RNDD, RNDD);
#undef RNDD
    to.add(r);
}

// check for critical
bool checkcrit(float dist, float m, int base = 0, int low = 4, int high = 100)
{
    return !m_real(gamemode, mutators) && !rnd((base + clamp(int(ceil(dist) * m), low, high)) * (m_classic(gamemode, mutators) ? 2 : 1));
}

// easy to send shot damage messages
inline void sendhit(client &actor, int gun, const vec &o, int dmg)
{
    // no blood or explosions if using moon jump
#if (SERVER_BUILTIN_MOD & 6) == 6 // 2 | 4
#if !(SERVER_BUILTIN_MOD & 4)
    if (m_gib(gamemode, mutators))
#endif
        return;
#endif
    sendf(NULL, 1, "ri7", SV_EXPLODE, actor.clientnum, gun, dmg, (int)(o.x*DMF), (int)(o.y*DMF), (int)(o.z*DMF));
}

inline void sendheadshot(const vec &from, const vec &to, int damage)
{
    sendf(NULL, 1, "ri8", SV_HEADSHOT, (int)(from.x*DMF), (int)(from.y*DMF), (int)(from.z*DMF), (int)(to.x*DMF), (int)(to.y*DMF), (int)(to.z*DMF), damage);
}

void parsepos(client &c, const vector<posinfo> &pos, vec &out_o, vec &out_head)
{
    const posinfo *info = NULL;
    loopv(pos) if (pos[i].cn == c.clientnum) { info = &pos[i]; break; }
    // position
    if (scl.lagtrust >= 2 && info) out_o = info->o;
    else out_o = c.state.o; // don't trust the client's position, or not provided
    // fix z
    out_o.z += PLAYERHEIGHT * c.state.crouchfactor(gamemillis);
    // head delta
    if (scl.lagtrust >= 1 && info && info->head.x > 0 && info->head.y > 0 && info->head.z > 0)
    {
        out_head = info->head;
        // sanity check (no insane headshot OPK)
        out_head.sub(out_o);
        if (out_head.magnitude() > 2) out_head.normalize().mul(2); // the center of our head cannot deviate from our neck more than 50 cm
        out_head.add(out_o);
    }
    // no match? not trusted? approximate a location for the head
    else out_head = vec(0, -.25f, .25f).rotate_around_z(c.y * RAD).add(out_o);
}

// explosions

// order the explosion hits by distance
struct explosivehit
{
    client *target, *owner;
    int damage, flags;
    float dist;
    vec o;

    static int compare(explosivehit *a, explosivehit *b)
    {
        // if there is more damage, the distance is closer, therefore move it up: (-a) - (-b) = b - a
        return b->damage - a->damage;
    }
};

// explosions call this to check
int radialeffect(client &owner, client &target, vector<explosivehit> &hits, const vec &o, int weap, bool gib, bool max_damage = false)
{
    vec hit_location = target.state.o, hit_location2 = target.state.o;
    hit_location.z += (PLAYERHEIGHT + PLAYERABOVEEYE) / 2.f;
    hit_location2.z += PLAYERHEIGHT * target.state.crouchfactor(gamemillis);
    // distance calculations
    float dist = max_damage ? 0 : min(hit_location.dist(o), hit_location2.dist(o));
    const bool useReciprocal = !m_classic(gamemode, mutators);
    if (dist >= (useReciprocal ? guns[weap].endrange : guns[weap].rangesub)) return 0; // too far away
    vec ray1(hit_location), ray2(hit_location2);
    ray1.sub(o).normalize();
    ray2.sub(o).normalize();
    if (srayclip(o, ray1) < dist && srayclip(o, ray2) < dist) return 0; // not visible
    float dmg = effectiveDamage(weap, dist, true, useReciprocal);
    int expflags = gib ? FRAG_GIB : FRAG_NONE;
    // check for critical
    if (checkcrit(dist, 2.5f)) // 1 : clamp(10 * meter, 4, 100) chance
    {
        expflags |= FRAG_CRIT;
        dmg *= 1.4f;
    }
    // did the nade headshot?
    if (weap == GUN_GRENADE && &owner != &target && o.z > hit_location2.z)
    {
        expflags |= FRAG_FLAG;
        sendheadshot(o, (hit_location = hit_location2), dmg);
        dmg *= 1.2f;
    }
    // was the RPG direct?
    else if (weap == GUN_RPG && max_damage)
        expflags |= FRAG_FLAG;
    explosivehit &hit = hits.add();
    hit.damage = (int)dmg;
    hit.flags = expflags;
    hit.target = &target;
    hit.owner = &owner;
    hit.dist = dist;
    hit.o = hit_location;
    return hit.damage;
}

// explosion call
int explosion(client &owner, const vec &o2, int weap, bool teamcheck, bool gib, client *cflag)
{
    int damagedealt = 0;
    vec o(o2);
    checkpos(o);
    sendhit(owner, weap, o, 0); // 0 means display explosion
    // these are our hits
    vector<explosivehit> hits;

    client *own_alt = NULL;
    // give credits to the shooter for killing the zombie!
    if (m_zombie(gamemode) && owner.team == TEAM_CLA && owner.state.revengelog.length() && valid_client(owner.state.revengelog.last()))
    {
        own_alt = clients[owner.state.revengelog.last()];
        if (own_alt->team != TEAM_RVSF)
            own_alt = NULL;
    }
    // suicide bomber's killer
    else if (weap == GUN_GRENADE && cflag && !isteam(cflag, &owner))
        own_alt = cflag;

    // find the hits
    loopv(clients)
    {
        client &target = *clients[i];
        if (target.type == ST_EMPTY || target.state.state != CS_ALIVE ||
            target.state.protect(gamemillis, gamemode, mutators)) continue;
        client *own = &owner;
        if (&owner != &target && isteam(&owner, &target))
        {
            if (own_alt)
                own = own_alt;
            else if (teamcheck)
                continue;
        }
        damagedealt += radialeffect(*own, target, hits, o, weap, gib, (weap == GUN_RPG && clients[i] == cflag));
    }
    if (m_overload(gamemode) && team_isactive(owner.team))
    {
        const int ot = team_opposite(owner.team);
        sflaginfo &f = sflaginfos[ot];
        vec flag_o(f.x, f.y, getsblock(getmaplayoutid((int)f.x, (int)f.y)).floor + (PLAYERHEIGHT + PLAYERABOVEEYE) / 2);
        const float dist = o.dist(flag_o);
        if (dist < PLAYERRADIUS * 4)
        {
            const int explosivedamage = guns[weap].damage >> 1; // half
            damagedealt += explosivedamage;

            sendf(NULL, 1, "ri2", SV_DAMAGEOBJECTIVE, owner.clientnum);
            f.damagetime = gamemillis;
            if ((f.damage += explosivedamage * (m_gsp1(gamemode, mutators) ? 16 : 8)) >= 255000)
            {
                f.damage = 0;
                flagaction(ot, FA_SCORE, owner.clientnum);
            }
        }
    }
    // sort the hits
    hits.sort(explosivehit::compare);
    // apply the hits
    loopv(hits)
    {
        sendhit(owner, weap, hits[i].o, hits[i].damage);
        serverdamage(*hits[i].target, *hits[i].owner, hits[i].damage, weap, hits[i].flags, o, hits[i].dist);
    }
    return damagedealt;
}

// order the nuke hits by distance
struct nukehit
{
    client *target;
    float distance;

    static int compare(nukehit *a, nukehit *b)
    {
        // less distance, so do it earlier
        if (a->distance < b->distance)
            return -1;
        // more distance, so do it later
        if (a->distance > b->distance)
            return 1;
        // same
        return 0;
    }
};

void nuke(client &owner, bool suicide, bool forced_all, bool friendly_fire)
{
    vector<nukehit> hits;
    loopvj(clients)
    {
        client *cl = clients[j];
        if (cl->type != ST_EMPTY && cl->team != TEAM_SPECT && cl != &owner && (friendly_fire || !isteam(cl, &owner)) && (forced_all || cl->state.state == CS_ALIVE))
        {
            // sort hits
            nukehit &hit = hits.add();
            hit.distance = cl->state.o.dist(owner.state.o);
            if (cl->type == ST_AI) hit.distance += 25 * CUBES_PER_METER; // to prioritize non-bots
            hit.target = cl;
        }
    }
    hits.sort(nukehit::compare);
    loopv(hits)
    {
        serverdied(*hits[i].target, owner, 0, OBIT_NUKE, !rnd(3) ? FRAG_GIB : FRAG_NONE, owner.state.o, hits[i].distance);
        // fx
        sendhit(owner, GUN_GRENADE, hits[i].target->state.o, 0);
    }
    // save the best for last!
    if (suicide)
    {
        owner.suicide(OBIT_NUKE, FRAG_NONE);
        // fx
        sendhit(owner, GUN_GRENADE, owner.state.o, 0);
    }
}

// Hitscans

struct shothit
{
    client *target;
    int damage, flags;
    float dist;
};

// hit checks
client *nearesthit(client &actor, const vec &from, const vec &to, bool teamcheck, int &hitzone, const vector<posinfo> &pos, vector<int> &exclude, vec &end, bool melee = false)
{
    client *result = NULL;
    float dist = 8e36f; // 2 undecillion meters
#define MELEE_PRECISION 11
    vec melees[MELEE_PRECISION];
    if (melee)
    {
        loopi(MELEE_PRECISION)
        {
            melees[i] = to;
            melees[i].sub(from);
            /*
            const float angle = ((i + 1.f) / MELEE_PRECISION - 0.5f) * 85.f * RAD; // from -85 to 85
            melees[i].rotate_around_x(angle * sinf(owner->aim[0]));
            melees[i].rotate_around_x(angle * cosf(owner->aim[0]));
            */
            melees[i].rotate_around_z(((i + 1.f) / MELEE_PRECISION - 0.5f) * 25.f * RAD); // from 25 to 25 (50 degrees)
            melees[i].add(from);
        }
    }
    loopv(clients)
    {
        client &t = *clients[i];
        clientstate &ts = t.state;
        // basic checks
        if (t.type == ST_EMPTY || ts.state != CS_ALIVE || exclude.find(i) >= 0 ||
            (teamcheck && &actor != &t && isteam(&actor, &t)) || ts.protect(gamemillis, gamemode, mutators)) continue;
        const float d = ts.o.dist(from);
        if (d > dist) continue;
        vec o, head;
        parsepos(t, pos, o, head);
        int hz = HIT_NONE;
        if (melee)
        {
            loopi(MELEE_PRECISION)
            {
                hz = hitplayer(from, actor.y, actor.p, melees[i], o, head, &end);
                if (hz) continue; // one of the knife rays hit
            }
            if (!hz) continue; // none of the knife rays hit
        }
        else
        {
            hz = hitplayer(from, actor.y, actor.p, to, o, head, &end);
            if (!hz) continue; // no hit
        }
        result = &t;
        dist = d;
        hitzone = hz;
    }
    return result;
}

// do a single line
int shot(client &owner, const vec &from, vec &to, const vector<posinfo> &pos, int weap, int style, const vec &surface, vector<int> &exclude, float dist = 0, float penaltydist = 0, vector<shothit> *save = NULL)
{
    const mul &mulset = muls[guns[weap].mulset];
    int hitzone = HIT_NONE; vec end = to;
    // out of range?
    if (melee_weap(weap))
    {
#if (SERVER_BUILTIN_MOD & 32)
        // super knife
        if (m_gib(gamemode, mutators))
        {
            static const int lulz[3] = { GUN_SNIPER, GUN_HEAL, GUN_RPG };
            sendf(NULL, 1, "ri9", SV_RICOCHET, owner.clientnum, lulz[rnd(3)], (int)(from.x*DMF), (int)(from.y*DMF), (int)(from.z*DMF), (int)(to.x*DMF), (int)(to.y*DMF), (int)(to.z*DMF));
        }
        else
#endif
        {
            // limit melee distance
            to.sub(from);
            if (to.magnitude() > guns[weap].endrange)
            {
                to.normalize().mul(guns[weap].endrange);
            }
            to.add(from);
        }
    }
    // calculate the hit
    client *hit = nearesthit(owner, from, to, !m_real(gamemode, mutators), hitzone, pos, exclude, end, melee_weap(weap) && (!(SERVER_BUILTIN_MOD & 1) || !m_gib(gamemode, mutators)));
    // damage check
    const float dist2 = dist + end.dist(from);
    int damage = effectiveDamage(weap, dist2 + penaltydist);
#if (SERVER_BUILTIN_MOD & 16)
    if (!dist)
    {
        // removed: red RPG shotline
        explosion(owner, end, GUN_RPG, !m_real(gamemode, mutators), false);
    }
#endif
    // we hit somebody
    if (hit && damage)
    {
        // damage multipliers
        if(guns[weap].mulset == MUL_PRO)
        {
            if (hitzone == HIT_HEAD)
                //damage *= mulset.head;
                ; // multiplying by 1 does nothing
            else
                damage = 0;
        }
        else if (!m_classic(gamemode, mutators) || hitzone >= HIT_HEAD || guns[weap].mulset == MUL_PRO2)
        {
            if (hitzone == HIT_HEAD)
                damage *= m_progressive(gamemode, mutators) ? 7 : mulset.head;
            else if (hitzone == HIT_TORSO)
                damage *= mulset.torso;
            // legs is always 1
        }
        // gib check
        if ((melee_weap(weap) || hitzone == HIT_HEAD) && !save) style |= FRAG_GIB;
        // critical shots
        if (checkcrit(dist2, 3.5f)) // 1 in clamp(14 * meter, 4, 100)
        {
            style |= FRAG_CRIT;
            damage *= 1.5f;
        }

        // melee weapons (bleed/check for self)
        if (melee_weap(weap))
        {
            if (hitzone == HIT_HEAD) style |= FRAG_FLAG;
            if (&owner == hit) return 0; // not possible
            else if (!isteam(&owner, hit)) // do not cause teammates to bleed
            {
                hit->state.addwound(owner.clientnum, end);
                sendf(NULL, 1, "ri2", SV_BLEED, hit->clientnum);
            }
        }

        // send bloody headshot hits...
        if (hitzone == HIT_HEAD) sendheadshot(from, end, damage);
        // send the real hit (blood fx)
        sendhit(owner, weap, end, damage);
        // apply damage
        if (save)
        {
            // save damage for shotgun rays
            shothit &h = save->add();
            h.target = hit;
            h.damage = damage;
            h.flags = style;
            h.dist = dist2;
        }
        else serverdamage(*hit, owner, damage, weap, style, from, dist2);

        // add hit to the exclude list
        exclude.add(hit->clientnum);

        // penetration: distort ray and continue through...
        vec dir(to = end), newsurface;
        // 5 degrees (both ways = 10 degrees) distortion on all axis
        dir.sub(from)
            .normalize()
            .rotate_around_x((rnd(11) - 5)*RAD)
            .rotate_around_y((rnd(11) - 5)*RAD)
            .rotate_around_z((rnd(11) - 5)*RAD)
            .add(end);
        // retrace
        straceShot(end, dir, &newsurface);
        const int penetratedamage = shot(owner, end, dir, pos, weap, style|FRAG_PENETRATE, newsurface, exclude, dist2, penaltydist + 10 * CUBES_PER_METER, save); // distance penalty for penetrating the player
        sendf(NULL, 1, "ri9", SV_RICOCHET, owner.clientnum, weap, (int)(end.x*DMF), (int)(end.y*DMF), (int)(end.z*DMF), (int)(dir.x*DMF), (int)(dir.y*DMF), (int)(dir.z*DMF));
        return damage + penetratedamage;
    }
    else
    {
        if (m_overload(gamemode) && team_isactive(owner.team))
        {
            const int ot = team_opposite(owner.team);
            sflaginfo &f = sflaginfos[ot];
            vec bottom(f.x, f.y, getsblock(getmaplayoutid((int)f.x, (int)f.y)).floor), top(bottom);
            top.z += PLAYERHEIGHT + PLAYERABOVEEYE;
            float dist;
            if (intersectcylinder(from, to, bottom, top, PLAYERRADIUS * 2, dist))
            {
                damage = effectiveDamage(weap, dist + penaltydist);
                to.sub(from).mul(dist).add(from);

                sendf(NULL, 1, "ri2", SV_DAMAGEOBJECTIVE, owner.clientnum);
                f.damagetime = gamemillis;
                if ((f.damage += damage * (m_gsp1(gamemode, mutators) ? 16 : 8)) >= 255000)
                {
                    f.damage = 0;
                    flagaction(ot, FA_SCORE, owner.clientnum);
                }

                return damage;
            }
        }
        // ricochet
        if (!dist && surface.magnitude()) // ricochet once if it came from the gun directly
        {
            // reset exclusion to the owner, so a penetrated player can be hit twice
            if (exclude.length() > 1)
                exclude.setsize(1);
            vec dir(to);
            // calculate reflected ray from incident ray and surface normal
            dir.sub(from).normalize();

            const float dotproduct = dir.dot(surface);
            if (fabs(dotproduct) > 0.96592582628f) // minimum angle is 15 degrees from normal
                return damage;

            // r = i - 2 (i . n) n
            dir.sub(vec(surface).mul(2 * dotproduct));
            // 2 degrees (both ways = 4 degrees) distortion on all axis
            dir
                .rotate_around_x((rnd(5) - 2)*RAD)
                .rotate_around_y((rnd(5) - 2)*RAD)
                .rotate_around_z((rnd(5) - 2)*RAD)
                .add(to);
            // retrace
            vec newsurface;
            straceShot(to, dir, &newsurface);
            const int ricochetdamage = shot(owner, to, dir, pos, weap, style|FRAG_RICOCHET, newsurface, exclude, dist2, penaltydist + 15 * CUBES_PER_METER, save); // distance penalty for ricochet
            sendf(NULL, 1, "ri9", SV_RICOCHET, owner.clientnum, weap, (int)(to.x*DMF), (int)(to.y*DMF), (int)(to.z*DMF), (int)(dir.x*DMF), (int)(dir.y*DMF), (int)(dir.z*DMF));
            return damage + ricochetdamage;
        }
    }
    return 0;
}

int shotgun(client &owner, const vec &from, vector<posinfo> &pos, int weap)
{
    int damagedealt = 0;
    clientstate &gs = owner.state;
    // many rays many hits, but we want each client to get all the damage at once...
    static vector<shothit> hits;
    hits.setsize(0);
    loopi(SGRAYS)
    {
        // check rays and sum damage
        vec surface;
        straceShot(from, gs.sg[i], &surface);
        static vector<int> exclude;
        exclude.setsize(0);
        exclude.add(owner.clientnum);
        shot(owner, from, gs.sg[i], pos, weap, FRAG_NONE, surface, exclude, 0, 0, &hits);
    }
    loopv(clients)
    {
        // apply damage
        client &t = *clients[i];
        clientstate &ts = t.state;
        // basic checks
        if (t.type == ST_EMPTY || ts.state != CS_ALIVE) continue;
        int damage = 0, shotgunflags = 0;
        float bestdist = 0;
        loopvrev(hits)
            if (hits[i].target == &t)
            {
                damage += hits[i].damage;
                shotgunflags |= hits[i].flags; // merge crit, etc.
                if (hits[i].dist > bestdist) bestdist = hits[i].dist;
                hits.remove(i/*--*/);
            }
        if (!damage) continue;
        damagedealt += damage;
        shotgunflags |= damage >= SGGIB * HEALTHSCALE ? FRAG_GIB : FRAG_NONE;
        if (m_progressive(gamemode, mutators) && shotgunflags & FRAG_GIB)
            damage = max(damage, 350 * HEALTHSCALE);
        serverdamage(t, owner, damage, weap, shotgunflags, from, bestdist);
    }
    return damagedealt;
}
